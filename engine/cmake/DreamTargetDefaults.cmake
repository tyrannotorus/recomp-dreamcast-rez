# Apply the project-wide compile settings to a target. Every dream target, and every translation unit
# of emitted game code, must go through here so that every host computes the same results:
#   - no floating-point contraction (an FMA on ARM64 but not on x86-64 changes results in the last
#     bit), hence -ffp-contract=off and /fp:strict;
#   - no compile-time evaluation under the compiler's default rounding: the guest runs with
#     FPSCR.RM = round-toward-zero and switches modes at run time, so a folded 1.0f/3.0f is wrong
#     in the last bit. Hence
#     -frounding-math and /fp:strict, which make the compiler assume a dynamic rounding mode;
#   - warnings on, and as errors by default, so portability problems surface on the first platform
#     that sees them rather than the third;
#   - no errno from math functions: nothing reads it, and keeping it turns FSRRA's square root into
#     a libm call on NaN, which costs every emitting function its callee-saved registers.
function(dream_target_defaults tgt)
  target_compile_features(${tgt} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE
      /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8
      /fp:strict /fp:except-
      /external:W0   # third-party headers (libchdr) are SYSTEM includes; do not warn on them
      $<$<BOOL:${DREAM_WERROR}>:/WX>)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${tgt} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
      -ffp-contract=off -frounding-math -fno-math-errno
      $<$<BOOL:${DREAM_WERROR}>:-Werror>)
  endif()
  set_target_properties(${tgt} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endfunction()
