#include "gtest/gtest.h"

extern "C" {
#include "h5zsperr_helper.h"
}

namespace {

TEST(h5zsperr_helper, pack_extra_info)
{
  // Test all possible combinations
  int rank = 0, is_float = 0, mode = 0;
  for (int r = 2; r <= 3; r++)
    for (int f = 0; f <= 1; f++)
      for (int m = 0; m <=4; m++) {
        rank = r * f * m + 10;
        is_float = rank + 10;
        mode = rank + 20;
        auto encode = C_API::h5zsperr_pack_extra_info(r, f, m);
        C_API::h5zsperr_unpack_extra_info(encode, &rank, &is_float, &mode);
        ASSERT_EQ(rank, r);
        ASSERT_EQ(is_float, f);
        ASSERT_EQ(mode, m);
      }
}

}
