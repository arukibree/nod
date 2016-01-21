#include <stdio.h>
#include <string.h>
#include "NOD/DiscWii.hpp"
#include "NOD/aes.hpp"

namespace NOD
{

static const uint8_t COMMON_KEYS[2][16] =
{
    /* Normal */
    {0xeb, 0xe4, 0x2a, 0x22,
     0x5e, 0x85, 0x93, 0xe4,
     0x48, 0xd9, 0xc5, 0x45,
     0x73, 0x81, 0xaa, 0xf7},
    /* Korean */
    {0x63, 0xb8, 0x2b, 0xb4,
     0xf4, 0x61, 0x4e, 0x2e,
     0x13, 0xf2, 0xfe, 0xfb,
     0xba, 0x4c, 0x9b, 0x7e}
};

class PartitionWii : public DiscBase::IPartition
{
    enum class SigType : uint32_t
    {
        RSA_4096 = 0x00010000,
        RSA_2048 = 0x00010001,
        ELIPTICAL_CURVE = 0x00010002
    };

    enum class KeyType : uint32_t
    {
        RSA_4096 = 0x00000000,
        RSA_2048 = 0x00000001
    };

    struct Ticket
    {
        uint32_t sigType;
        char sig[256];
        char padding[60];
        char sigIssuer[64];
        char ecdh[60];
        char padding1[3];
        unsigned char encKey[16];
        char padding2;
        char ticketId[8];
        char consoleId[4];
        char titleId[8];
        char padding3[2];
        uint16_t ticketVersion;
        uint32_t permittedTitlesMask;
        uint32_t permitMask;
        char titleExportAllowed;
        char commonKeyIdx;
        char padding4[48];
        char contentAccessPermissions[64];
        char padding5[2];
        struct TimeLimit
        {
            uint32_t enableTimeLimit;
            uint32_t timeLimit;
        } timeLimits[8];

        void read(IDiscIO::IReadStream& s)
        {
            s.read(this, 676);
            sigType = SBig(sigType);
            ticketVersion = SBig(ticketVersion);
            permittedTitlesMask = SBig(permittedTitlesMask);
            permitMask = SBig(permitMask);
            for (size_t t=0 ; t<8 ; ++t)
            {
                timeLimits[t].enableTimeLimit = SBig(timeLimits[t].enableTimeLimit);
                timeLimits[t].timeLimit = SBig(timeLimits[t].timeLimit);
            }
        }
    } m_ticket;

    struct TMD
    {
        SigType sigType;
        char sig[256];
        char padding[60];
        char sigIssuer[64];
        char version;
        char caCrlVersion;
        char signerCrlVersion;
        char padding1;
        uint32_t iosIdMajor;
        uint32_t iosIdMinor;
        uint32_t titleIdMajor;
        char titleIdMinor[4];
        uint32_t titleType;
        uint16_t groupId;
        char padding2[62];
        uint32_t accessFlags;
        uint16_t titleVersion;
        uint16_t numContents;
        uint16_t bootIdx;
        uint16_t padding3;

        struct Content
        {
            uint32_t id;
            uint16_t index;
            uint16_t type;
            uint64_t size;
            char hash[20];

            void read(IDiscIO::IReadStream& s)
            {
                s.read(this, 36);
                id = SBig(id);
                index = SBig(index);
                type = SBig(type);
                size = SBig(size);
            }
        };
        std::vector<Content> contents;

        void read(IDiscIO::IReadStream& s)
        {
            s.read(this, 484);
            sigType = SigType(SBig(uint32_t(sigType)));
            iosIdMajor = SBig(iosIdMajor);
            iosIdMinor = SBig(iosIdMinor);
            titleIdMajor = SBig(titleIdMajor);
            titleType = SBig(titleType);
            groupId = SBig(groupId);
            accessFlags = SBig(accessFlags);
            titleVersion = SBig(titleVersion);
            numContents = SBig(numContents);
            bootIdx = SBig(bootIdx);
            contents.clear();
            contents.reserve(numContents);
            for (uint16_t c=0 ; c<numContents ; ++c)
            {
                contents.emplace_back();
                contents.back().read(s);
            }
        }
    } m_tmd;

    struct Certificate
    {
        SigType sigType;
        char sig[512];
        char issuer[64];
        KeyType keyType;
        char subject[64];
        char key[512];
        uint32_t modulus;
        uint32_t pubExp;

        void read(IDiscIO::IReadStream& s)
        {
            s.read(&sigType, 4);
            sigType = SigType(SBig(uint32_t(sigType)));
            if (sigType == SigType::RSA_4096)
                s.read(sig, 512);
            else if (sigType == SigType::RSA_2048)
                s.read(sig, 256);
            else if (sigType == SigType::ELIPTICAL_CURVE)
                s.read(sig, 64);
            s.seek(60, SEEK_CUR);

            s.read(issuer, 64);
            s.read(&keyType, 4);
            s.read(subject, 64);
            keyType = KeyType(SBig(uint32_t(keyType)));
            if (keyType == KeyType::RSA_4096)
                s.read(key, 512);
            else if (keyType == KeyType::RSA_2048)
                s.read(key, 256);

            s.read(&modulus, 8);
            modulus = SBig(modulus);
            pubExp = SBig(pubExp);

            s.seek(52, SEEK_CUR);
        }
    };
    Certificate m_caCert;
    Certificate m_tmdCert;
    Certificate m_ticketCert;

    uint64_t m_dataOff;
    uint8_t m_decKey[16];

public:
    PartitionWii(const DiscWii& parent, Kind kind, uint64_t offset)
    : IPartition(parent, kind, offset)
    {
        std::unique_ptr<IDiscIO::IReadStream> s = parent.getDiscIO().beginReadStream(offset);
        m_ticket.read(*s);

        uint32_t tmdSize;
        s->read(&tmdSize, 4);
        tmdSize = SBig(tmdSize);
        uint32_t tmdOff;
        s->read(&tmdOff, 4);
        tmdOff = SBig(tmdOff) << 2;

        uint32_t certChainSize;
        s->read(&certChainSize, 4);
        certChainSize = SBig(certChainSize);
        uint32_t certChainOff;
        s->read(&certChainOff, 4);
        certChainOff = SBig(certChainOff) << 2;

        uint32_t globalHashTableOff;
        s->read(&globalHashTableOff, 4);
        globalHashTableOff = SBig(globalHashTableOff) << 2;

        uint32_t dataOff;
        s->read(&dataOff, 4);
        dataOff = SBig(dataOff) << 2;
        m_dataOff = offset + dataOff;
        uint32_t dataSize;
        s->read(&dataSize, 4);
        dataSize = SBig(dataSize) << 2;

        s->seek(offset + tmdOff);
        m_tmd.read(*s);

        s->seek(offset + certChainOff);
        m_caCert.read(*s);
        m_tmdCert.read(*s);
        m_ticketCert.read(*s);

        /* Decrypt title key */
        std::unique_ptr<IAES> aes = NewAES();
        uint8_t iv[16] = {};
        memcpy(iv, m_ticket.titleId, 8);
        aes->setKey(COMMON_KEYS[(int)m_ticket.commonKeyIdx]);
        aes->decrypt(iv, m_ticket.encKey, m_decKey, 16);

        /* Wii-specific header reads (now using title key to decrypt) */
        std::unique_ptr<IPartReadStream> ds = beginReadStream(0x420);
        uint32_t vals[3];
        ds->read(vals, 12);
        m_dolOff = SBig(vals[0]) << 2;
        m_fstOff = SBig(vals[1]) << 2;
        m_fstSz = SBig(vals[2]) << 2;
        ds->seek(0x2440 + 0x14);
        ds->read(vals, 8);
        m_apploaderSz = 32 + SBig(vals[0]) + SBig(vals[1]);

        /* Yay files!! */
        parseFST(*ds);

        /* Also make DOL header and size handy */
        ds->seek(m_dolOff);
        parseDOL(*ds);
    }

    class PartReadStream : public IPartReadStream
    {
        std::unique_ptr<IAES> m_aes;
        const PartitionWii& m_parent;
        uint64_t m_baseOffset;
        uint64_t m_offset;
        std::unique_ptr<IDiscIO::IReadStream> m_dio;

        size_t m_curBlock = SIZE_MAX;
        uint8_t m_encBuf[0x8000];
        uint8_t m_decBuf[0x7c00];

        void decryptBlock()
        {
            m_dio->read(m_encBuf, 0x8000);
            m_aes->decrypt(&m_encBuf[0x3d0], &m_encBuf[0x400], m_decBuf, 0x7c00);
        }
    public:
        PartReadStream(const PartitionWii& parent, uint64_t baseOffset, uint64_t offset)
        : m_aes(NewAES()), m_parent(parent), m_baseOffset(baseOffset), m_offset(offset)
        {
            m_aes->setKey(parent.m_decKey);
            size_t block = m_offset / 0x7c00;
            m_dio = m_parent.m_parent.getDiscIO().beginReadStream(m_baseOffset + block * 0x8000);
            decryptBlock();
            m_curBlock = block;
        }
        void seek(int64_t offset, int whence)
        {
            if (whence == SEEK_SET)
                m_offset = offset;
            else if (whence == SEEK_CUR)
                m_offset += offset;
            else
                return;
            size_t block = m_offset / 0x7c00;
            if (block != m_curBlock)
            {
                m_dio->seek(m_baseOffset + block * 0x8000);
                decryptBlock();
                m_curBlock = block;
            }
        }
        uint64_t position() const {return m_offset;}
        uint64_t read(void* buf, uint64_t length)
        {
            size_t block = m_offset / 0x7c00;
            size_t cacheOffset = m_offset % 0x7c00;
            uint64_t cacheSize;
            uint64_t rem = length;
            uint8_t* dst = (uint8_t*)buf;

            while (rem)
            {
                if (block != m_curBlock)
                {
                    decryptBlock();
                    m_curBlock = block;
                }

                cacheSize = rem;
                if (cacheSize + cacheOffset > 0x7c00)
                    cacheSize = 0x7c00 - cacheOffset;

                memcpy(dst, m_decBuf + cacheOffset, cacheSize);
                dst += cacheSize;
                rem -= cacheSize;
                cacheOffset = 0;
                ++block;
            }

            m_offset += length;
            return dst - (uint8_t*)buf;
        }
    };

    std::unique_ptr<IPartReadStream> beginReadStream(uint64_t offset) const
    {
        return std::unique_ptr<IPartReadStream>(new PartReadStream(*this, m_dataOff, offset));
    }

    uint64_t normalizeOffset(uint64_t anOffset) const {return anOffset << 2;}
};

DiscWii::DiscWii(std::unique_ptr<IDiscIO>&& dio)
: DiscBase(std::move(dio))
{
    /* Read partition info */
    struct PartInfo
    {
        uint32_t partCount;
        uint32_t partInfoOff;
        struct Part
        {
            uint32_t partDataOff;
            IPartition::Kind partType;
        } parts[4];
        PartInfo(IDiscIO& dio)
        {
            std::unique_ptr<IDiscIO::IReadStream> s = dio.beginReadStream(0x40000);
            s->read(this, 32);
            partCount = SBig(partCount);
            partInfoOff = SBig(partInfoOff);

            s->seek(partInfoOff << 2);
            for (uint32_t p=0 ; p<partCount && p<4 ; ++p)
            {
                s->read(&parts[p], 8);
                parts[p].partDataOff = SBig(parts[p].partDataOff);
                parts[p].partType = IPartition::Kind(SBig(uint32_t(parts[p].partType)));
            }
        }
    } partInfo(*m_discIO);

    /* Iterate for data partition */
    m_partitions.reserve(partInfo.partCount);
    for (uint32_t p=0 ; p<partInfo.partCount && p<4 ; ++p)
    {
        PartInfo::Part& part = partInfo.parts[p];
        IPartition::Kind kind;
        switch (part.partType)
        {
        case IPartition::Kind::Data:
        case IPartition::Kind::Update:
        case IPartition::Kind::Channel:
            kind = part.partType;
            break;
        default:
            LogModule.report(LogVisor::FatalError, "invalid partition type %d", part.partType);
        }
        m_partitions.emplace_back(new PartitionWii(*this, kind, part.partDataOff << 2));
    }
}

}
