#include <cstdint>
#include <cstdio>
#include <vector>

#include "lzokay.hpp"

namespace {

constexpr std::uint32_t kPackageTag = 0x9E2A83C1u;
constexpr long kCompressionTableOffset = 0x71;

struct PackageChunk {
    std::uint32_t uncompressedOffset;
    std::uint32_t uncompressedSize;
    std::uint32_t compressedOffset;
    std::uint32_t compressedSize;
};

struct BlockSize {
    std::uint32_t compressed;
    std::uint32_t uncompressed;
};

template <typename T>
bool Read(FILE* file, T* value) {
    return std::fread(value, sizeof(*value), 1, file) == 1;
}

bool Seek(FILE* file, std::uint32_t offset) {
    return _fseeki64(file, offset, SEEK_SET) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: DecompressKidPackage <input.u> <output.u>\n");
        return 2;
    }

    FILE* input = nullptr;
    FILE* output = nullptr;
    if (fopen_s(&input, argv[1], "rb") != 0 || input == nullptr ||
        fopen_s(&output, argv[2], "wb+") != 0 || output == nullptr) {
        std::fprintf(stderr, "could not open input or output\n");
        if (input != nullptr) std::fclose(input);
        if (output != nullptr) std::fclose(output);
        return 3;
    }

    int result = 1;
    do {
        if (_fseeki64(input, kCompressionTableOffset, SEEK_SET) != 0) break;

        std::uint32_t chunkCount = 0;
        if (!Read(input, &chunkCount) || chunkCount == 0 || chunkCount > 4096) break;

        std::vector<PackageChunk> chunks(chunkCount);
        if (std::fread(chunks.data(), sizeof(PackageChunk), chunks.size(), input) != chunks.size()) break;

        std::vector<std::uint8_t> header(chunks.front().uncompressedOffset);
        if (_fseeki64(input, 0, SEEK_SET) != 0 ||
            std::fread(header.data(), 1, header.size(), input) != header.size() ||
            std::fwrite(header.data(), 1, header.size(), output) != header.size()) break;

        for (std::uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            const PackageChunk& chunk = chunks[chunkIndex];
            if (!Seek(input, chunk.compressedOffset) || !Seek(output, chunk.uncompressedOffset)) break;

            std::uint32_t tag = 0;
            std::uint32_t blockSize = 0;
            BlockSize summary = {};
            if (!Read(input, &tag) || !Read(input, &blockSize) || !Read(input, &summary) ||
                tag != kPackageTag || summary.uncompressed != chunk.uncompressedSize || blockSize == 0) {
                std::fprintf(stderr, "invalid compressed chunk %u\n", chunkIndex);
                break;
            }

            const std::uint32_t blockCount =
                (summary.uncompressed + blockSize - 1) / blockSize;
            std::vector<BlockSize> blocks(blockCount);
            if (std::fread(blocks.data(), sizeof(BlockSize), blocks.size(), input) != blocks.size()) break;

            bool chunkOkay = true;
            for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
                const BlockSize& block = blocks[blockIndex];
                std::vector<std::uint8_t> compressed(block.compressed);
                std::vector<std::uint8_t> decompressed(block.uncompressed);
                if (std::fread(compressed.data(), 1, compressed.size(), input) != compressed.size()) {
                    chunkOkay = false;
                    break;
                }

                std::size_t outputSize = 0;
                const lzokay::EResult status = lzokay::decompress(
                    compressed.data(), compressed.size(), decompressed.data(),
                    decompressed.size(), outputSize);
                if (status != lzokay::EResult::Success || outputSize != decompressed.size()) {
                    std::fprintf(stderr,
                        "LZO failure in chunk %u block %u: status %d, output %zu/%zu\n",
                        chunkIndex, blockIndex, static_cast<int>(status), outputSize,
                        decompressed.size());
                    chunkOkay = false;
                    break;
                }
                if (std::fwrite(decompressed.data(), 1, decompressed.size(), output) != decompressed.size()) {
                    chunkOkay = false;
                    break;
                }
            }

            if (!chunkOkay) break;
            if (chunkIndex + 1 == chunkCount) {
                result = 0;
            }
        }
    } while (false);

    std::fclose(output);
    std::fclose(input);
    return result;
}
