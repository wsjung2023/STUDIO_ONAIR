#include "core/Sha256.h"

#include "core/AppError.h"

#include <algorithm>
#include <bit>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace creator::core {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

}  // namespace

void Sha256::update(std::span<const std::uint8_t> bytes) {
    totalBytes_ += bytes.size();
    while (!bytes.empty()) {
        const std::size_t copied =
            std::min(bytes.size(), buffer_.size() - buffered_);
        std::copy_n(bytes.begin(), copied, buffer_.begin() + buffered_);
        buffered_ += copied;
        bytes = bytes.subspan(copied);
        if (buffered_ == buffer_.size()) {
            transform(buffer_);
            buffered_ = 0;
        }
    }
}

std::string Sha256::finish() {
    const std::uint64_t totalBits = totalBytes_ * 8U;
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        std::fill(buffer_.begin() + buffered_, buffer_.end(), std::uint8_t{0});
        transform(buffer_);
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + buffered_, buffer_.begin() + 56,
              std::uint8_t{0});
    for (int index = 0; index < 8; ++index) {
        buffer_[63 - index] = static_cast<std::uint8_t>(
            totalBits >> static_cast<unsigned>(index * 8));
    }
    transform(buffer_);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : state_) output << std::setw(8) << word;
    return output.str();
}

void Sha256::transform(const std::array<std::uint8_t, 64>& block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t offset = index * 4;
        words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                       (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                       (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                       static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const auto s0 = std::rotr(words[index - 15], 7) ^
                        std::rotr(words[index - 15], 18) ^
                        (words[index - 15] >> 3U);
        const auto s1 = std::rotr(words[index - 2], 17) ^
                        std::rotr(words[index - 2], 19) ^
                        (words[index - 2] >> 10U);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                          std::rotr(e, 25);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto temporary1 =
            h + sum1 + choose + kRoundConstants[index] + words[index];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                          std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

Result<std::string> sha256File(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return AppError{ErrorCode::IoFailure,
                        "could not read file for SHA-256"};
    }
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(buffer.data()),
                static_cast<std::size_t>(count)});
        }
    }
    if (!input.eof()) {
        return AppError{ErrorCode::IoFailure,
                        "could not hash file with SHA-256"};
    }
    return hash.finish();
}

}  // namespace creator::core
