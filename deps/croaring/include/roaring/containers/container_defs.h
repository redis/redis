/*
 * container_defs.h
 *
 * Unlike containers.h (which is a file aggregating all the container includes,
 * like array.h, bitset.h, and run.h) this is a file included BY those headers
 * to do things like define the container base class `container_t`.
 */

#ifndef INCLUDE_CONTAINERS_CONTAINER_DEFS_H_
#define INCLUDE_CONTAINERS_CONTAINER_DEFS_H_

#ifdef __cplusplus
#include <type_traits>  // used by casting helper for compile-time check
#endif

// The preferences are a separate file to separate out tweakable parameters
#include <roaring/containers/perfparameters.h>

#ifdef __cplusplus
namespace roaring {
namespace internal {  // No extern "C" (contains template)
#endif

/*
 * Since roaring_array_t's definition is not opaque, the container type is
 * part of the API.  If it's not going to be `void*` then it needs a name, and
 * expectations are to prefix C library-exported names with `roaring_` etc.
 *
 * Rather than force the whole codebase to use the name `roaring_container_t`,
 * the few API appearances use the macro ROARING_CONTAINER_T.  Those includes
 * are prior to containers.h, so make a short private alias of `container_t`.
 * Then undefine the awkward macro so it's not used any more than it has to be.
 */
typedef ROARING_CONTAINER_T container_t;

/*
 * See ROARING_CONTAINER_T for notes on using container_t as a base class.
 * This macro helps make the following pattern look nicer:
 *
 *     #ifdef __cplusplus
 *     struct roaring_array_s : public container_t {
 *     #else
 *     struct roaring_array_s {
 *     #endif
 *         int32_t cardinality;
 *         int32_t capacity;
 *         uint16_t *array;
 *     }
 */
#if defined(__cplusplus)
#define STRUCT_CONTAINER(name) struct name : public container_t /* { ... } */
#else
#define STRUCT_CONTAINER(name) struct name /* { ... } */
#endif

/**
 * Since container_t* is not void* in C++, "dangerous" casts are not needed to
 * downcast; only a static_cast<> is needed.  Define a macro for static casting
 * which helps make casts more visible, and catches problems at compile-time
 * when building the C sources in C++ mode:
 *
 *     void some_func(container_t **c, ...) {  // double pointer, not single
 *         array_container_t *ac1 = (array_container_t *)(c);  // uncaught!!
 *
 *         array_container_t *ac2 = CAST(array_container_t *, c)  // C++ errors
 *         array_container_t *ac3 = CAST_array(c);  // shorthand for #2, errors
 *     }
 *
 * Trickier to do is a cast from `container**` to `array_container_t**`.  This
 * needs a reinterpret_cast<>, which sacrifices safety...so a template is used
 * leveraging <type_traits> to make sure it's legal in the C++ build.
 */
#ifdef __cplusplus
#define CAST(type, value) static_cast<type>(value)
#define movable_CAST(type, value) movable_CAST_HELPER<type>(value)

template <typename PPDerived, typename Base>
PPDerived movable_CAST_HELPER(Base **ptr_to_ptr) {
    typedef typename std::remove_pointer<PPDerived>::type PDerived;
    typedef typename std::remove_pointer<PDerived>::type Derived;
    static_assert(std::is_base_of<Base, Derived>::value,
                  "use movable_CAST() for container_t** => xxx_container_t**");
    return reinterpret_cast<Derived **>(ptr_to_ptr);
}
#else
#define CAST(type, value) ((type)value)
#define movable_CAST(type, value) ((type)value)
#endif

// Use for converting e.g. an `array_container_t**` to a `container_t**`
//
#define movable_CAST_base(c) movable_CAST(container_t **, c)

#ifdef __cplusplus
}
}  // namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_CONTAINER_DEFS_H_ */
