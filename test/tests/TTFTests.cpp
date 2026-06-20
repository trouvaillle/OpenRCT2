/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <gtest/gtest.h>
#include <openrct2/drawing/TTF.h>

class TTFTests : public testing::Test
{
};

TEST_F(TTFTests, reinitialise_before_init_does_not_crash)
{
    // TTF is not initialised at this point, so TTFReinitialise should be a no-op.
    // This guards against crashes if TriggerResize fires before TryLoadFonts.
    EXPECT_NO_FATAL_FAILURE(TTFReinitialise());
}
