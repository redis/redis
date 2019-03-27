
  const char features[] = {"\n"
"C_FEATURE:"
#if ((__clang_major__ * 100) + __clang_minor__) >= 400
"1"
#else
"0"
#endif
"c_function_prototypes\n"
"C_FEATURE:"
#if ((__clang_major__ * 100) + __clang_minor__) >= 400 && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
"1"
#else
"0"
#endif
"c_restrict\n"
"C_FEATURE:"
#if ((__clang_major__ * 100) + __clang_minor__) >= 400 && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
"1"
#else
"0"
#endif
"c_static_assert\n"
"C_FEATURE:"
#if ((__clang_major__ * 100) + __clang_minor__) >= 400 && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
"1"
#else
"0"
#endif
"c_variadic_macros\n"

};

int main(int argc, char** argv) { (void)argv; return features[argc]; }
