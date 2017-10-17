#include "xPlatAppx.hpp"
#include "Exceptions.hpp"
#include "StreamBase.hpp"
#include "FileStream.hpp"
#include "RangeStream.hpp"
#include "ZipObject.hpp"
#include "DirectoryObject.hpp"

#include <string>
#include <memory>
#include <functional>


//typedef void *BCRYPT_ALG_HANDLE;
//typedef void *BCRYPT_HASH_HANDLE;


#define MIN_DIGEST_COUNT          5           // All digests except code integrity
#define MAX_DIGEST_COUNT          6           // Including code integrity
#define ID_SIZE                   4           // IDs are 4 bytes
#define SHA_256_DIGEST_SIZE       32
#define SMALL_INDIRECT_DATA_SIZE  (ID_SIZE + (MIN_DIGEST_COUNT * (SHA_256_DIGEST_SIZE + ID_SIZE)))
#define LARGE_INDIRECT_DATA_SIZE  (ID_SIZE + (MAX_DIGEST_COUNT * (SHA_256_DIGEST_SIZE + ID_SIZE)))
#define CI_AND_SIG_DATA_SIZE      36
#define HEADER_BEGINNING_SIZE     38
#define FOUR_MB                   4194304     

//
// Magic Values
//
#define INDIRECT_DATA_ID          0x58504145  // EAPX
#define PACKAGE_HEADER_ID         0x48505845  // EXPH
#define BUNDLE_HEADER_ID          0x48425845  // EXBH
#define SIGNATURE_ID              0x58434B50  // PKCX
#define AXEH                      0x48455841  // Encrypted Appx Header
#define AXEF                      0x46455841  // Encrypted Appx Footer
#define AXEB                      0x42455841  // Encrypted Appx Block Map
#define AXPC                      0x43505841  // Encrypted Appx Package Content
#define AXBM                      0x4D425841  // Unencrypted Block Map
#define AXCI                      0x49435841  // Encrypted Appx Code Integrity
#define AXEH_IDX                  0
#define AXEF_IDX                  1
#define AXEB_IDX                  2
#define AXPC_IDX                  3
#define AXBM_IDX                  4
#define AXCI_IDX                  5

#define MAX_SUPPORTED_EAPPX_VERSION     0x0001000000000000  // 1.0.0.0

// Wrapper for BCRYPT hash handles 
#ifdef DISABLE
//typedef struct _SHA256_HANDLE
{
BCRYPT_ALG_HANDLE   hAlgorithm;
BCRYPT_HASH_HANDLE  hHash;
} SHA256_HANDLE, *PSHA256_HANDLE;
#endif
//
// The structure for relevant info for one hash.
//
typedef struct _INDIRECT_DATA_DIGEST
{
    std::uint32_t id;
    std::uint64_t start;
    std::uint64_t size;
    std::uint8_t value[SHA_256_DIGEST_SIZE];
} INDIRECT_DATA_DIGEST;

//
// The structure that holds all hash data.
// 
typedef struct _EAPPX_INDIRECT_DATA
{
    std::uint32_t eappxId;
    std::uint8_t digestCount;
    INDIRECT_DATA_DIGEST digests[MAX_DIGEST_COUNT];
} EAPPX_INDIRECT_DATA;

#include <pshpack1.h>
typedef struct _BLOBHEADER
{
    std::uint32_t headerId;
    std::uint16_t headerSize;
    std::uint64_t version;
    std::uint64_t footerOffset;
    std::uint64_t footerSize;
    std::uint64_t fileCount;

    std::uint64_t signatureOffset;
    std::uint16_t signatureCompressionType;
    std::uint32_t signatureUncompressedSize;
    std::uint32_t signatureCompressedSize;

    std::uint64_t codeIntegrityOffset;
    std::uint16_t codeIntegrityCompressionType;
    std::uint32_t codeIntegrityUncompressedSize;
    std::uint32_t codeIntegrityCompressedSize;
} BLOBHEADER, *PBLOBHEADER;
#include <poppack.h>


// on apple platforms, compile with -fvisibility=hidden
#ifdef PLATFORM_APPLE
#undef XPLATAPPX_API
#define XPLATAPPX_API __attribute__((visibility("default")))

// Initializer.
__attribute__((constructor))
static void initializer(void) {                             // 2
    printf("[%s] initializer()\n", __FILE__);
}

// Finalizer.
__attribute__((destructor))
static void finalizer(void) {                               // 3
    printf("[%s] finalizer()\n", __FILE__);
}

#endif

// Provides an ABI exception boundary with parameter validation
using Lambda = std::function<void()>;

unsigned int ResultOf(char* source, char* destination, Lambda lambda)
{
    unsigned int result = 0;
    try
    {
        if (source == nullptr || destination == nullptr)
        {
            throw xPlat::InvalidArgumentException();
        }
        lambda();
    }
    catch (xPlat::ExceptionBase& exception)
    {
        result = exception.Code();
    }

    return result;
}

unsigned int ResultOf(char* appx, Lambda lambda)
{
    unsigned int result = 0;
    try
    {
        if (appx == nullptr)
        {
            throw xPlat::InvalidArgumentException();
        }
        lambda();
    }
    catch (xPlat::ExceptionBase& exception)
    {
        result = exception.Code();
    }

    return result;
}

XPLATAPPX_API unsigned int UnpackAppx(char* from, char* to)
{
    return ResultOf(from, to, [&]() {
        xPlat::DirectoryObject directory(to);
        auto rawFile = std::make_unique<xPlat::FileStream>(from, xPlat::FileStream::Mode::READ);

        {
            xPlat::ZipObject zip(rawFile.get());

            auto fileNames = zip.GetFileNames();
            for (const auto& fileName : fileNames)
            {
                auto sourceFile = zip.GetFile(fileName);
                auto targetFile = directory.OpenFile(fileName, xPlat::FileStream::Mode::WRITE_UPDATE);

                sourceFile->CopyTo(targetFile.get());
                targetFile->Close();
            }
        }
    });
}

XPLATAPPX_API unsigned int PackAppx  (char* from, char* to)
{
    return ResultOf(from, to, [&]() {
        xPlat::DirectoryObject directory(from);
        auto rawFile = std::make_unique<xPlat::FileStream>(to, xPlat::FileStream::Mode::WRITE);

        {
            xPlat::ZipObject zip(rawFile.get());

            auto fileNames = directory.GetFileNames();
            for (const auto& fileName : fileNames)
            {
                auto sourceFile = directory.GetFile(fileName);
                auto targetFile = zip.OpenFile(fileName, xPlat::FileStream::Mode::WRITE);

                sourceFile->CopyTo(targetFile.get());
            }
            zip.CommitChanges();
        }
    });
}


XPLATAPPX_API unsigned int ValidateAppxSignature(char* appx)
{
    return ResultOf(appx, [&]() {
        auto rawFile = std::make_unique<xPlat::FileStream>(appx, xPlat::FileStream::Mode::READ);

        {
            xPlat::ZipObject zip(rawFile.get());
            auto p7xStream = zip.GetFile("AppxSignature.p7x");
            std::uint8_t buffer[16384];
            
            std::size_t cbRead = p7xStream->Read(sizeof(buffer), buffer);
            BLOBHEADER *pblob = (BLOBHEADER*)buffer;
            
            if (cbRead > sizeof(BLOBHEADER) && pblob->headerId != SIGNATURE_ID)
            {
                throw xPlat::InvalidStreamFormat();
            }
            
            //auto rangeStream = std::make_unique<xPlat::RangeStream>(p7xStream, sizeof(P7xFileId), cbStream - sizeof(P7xFileId));
            //auto tempStream = std::make_unique<xPlat::FileStream>("e:\\temp\\temp.p7x", xPlat::FileStream::WRITE);
            //rangeStream->CopyTo(tempStream.get());
            //tempStream->Close();
        }
    });
}