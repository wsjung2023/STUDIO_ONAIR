#include "capture/AudioPcmLayout.h"

#include <gtest/gtest.h>

namespace {

using creator::capture::Float32PcmLayout;
using creator::capture::validateFloat32PcmLayout;

TEST(AudioPcmLayoutTest, AcceptsExactPackedInterleavedAndPlanarLayouts) {
    EXPECT_TRUE(validateFloat32PcmLayout(
                    {.channels = 2, .bytesPerFrame = 8, .interleaved = true,
                     .packed = true, .bigEndian = false})
                    .hasValue());
    EXPECT_TRUE(validateFloat32PcmLayout(
                    {.channels = 2, .bytesPerFrame = 4, .interleaved = false,
                     .packed = true, .bigEndian = false})
                    .hasValue());
}

TEST(AudioPcmLayoutTest, RejectsPaddedUnpackedAndBigEndianLayouts) {
    EXPECT_FALSE(validateFloat32PcmLayout(
                     {.channels = 2, .bytesPerFrame = 16, .interleaved = true,
                      .packed = true, .bigEndian = false})
                     .hasValue());
    EXPECT_FALSE(validateFloat32PcmLayout(
                     {.channels = 2, .bytesPerFrame = 8, .interleaved = true,
                      .packed = false, .bigEndian = false})
                     .hasValue());
    EXPECT_FALSE(validateFloat32PcmLayout(
                     {.channels = 2, .bytesPerFrame = 8, .interleaved = true,
                      .packed = true, .bigEndian = true})
                     .hasValue());
}

}  // namespace
