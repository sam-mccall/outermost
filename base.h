#ifndef BASE_H_
#define BASE_H_

#include <experimental/optional>
#include <experimental/string_view>

using u8 = unsigned char;
using u32 = uint32_t;
using string_view = std::experimental::string_view;
#define UNLIKELY(x) (__builtin_expect(x, 0))
#define LIKELY(x) (__builtin_expect(!!(x), 1))

#endif
