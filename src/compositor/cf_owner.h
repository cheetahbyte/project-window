#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <memory>
#include <type_traits>

namespace pw {

struct CfRelease {
    void operator()(CFTypeRef object) const { CFRelease(object); }
};

// Owns one reference to a CoreFoundation object, such as one returned by a Create function.
template <class Ref>
using CfOwner = std::unique_ptr<std::remove_pointer_t<Ref>, CfRelease>;

}  // namespace pw
