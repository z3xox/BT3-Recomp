include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# [optcfg] Which configs the tuned flags below apply to.
#
# This MUST match every config the caller enables optimization for. ps2xRuntime/CMakeLists.txt
# calls EnableFastReleaseMode for Release AND RelWithDebInfo, but every flag here used to be
# gated on $<CONFIG:Release> alone -- and the shipped Windows preset (CMakeSettings.json
# "x64-Release") is configurationType RelWithDebInfo. So on Windows the function was called,
# printed its "Enabling optimization" message, and then silently applied NOTHING: the build
# fell back to CMake's default RelWithDebInfo flags, /Zi /O2 /Ob1 /DNDEBUG.
#
# /Ob1 only inlines functions marked `inline`. The 7,800 generated guest-code files are wall to
# wall tiny helpers (guest register and memory accessors), so losing /Ob2 there is expensive --
# the Linux build compiles the same files at -O3. That is a Windows-only handicap on every
# machine regardless of its hardware, which is the shape of the "low fps on Windows" reports.
set(PS2X_OPT_CONFIGS $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    # clang-cl (the toolset the Windows build uses, see games/bt3/setup.py) reports MSVC=TRUE but does not
    # accept MSVC's /Qspectre- and /GL (whole-program LTCG), and its LTO objects need lld-link: give it the
    # plain optimization subset and no LTO, like the GCC/Clang builds on Linux.
    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Same numeric contract as the Linux build: SSE4.1 only (no FMA contraction -- the VU/clipper
        # math must round twice), no fast-math (Inf/NaN saturation), guest-memory type punning and
        # integer wraparound allowed.
        target_compile_options(${TargetName} PRIVATE
            $<${PS2X_OPT_CONFIGS}:/O2 /Ob2 /Oi /Gy /Gw /GF /DNDEBUG /GS- /fp:precise>
            /clang:-msse4.1 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-fwrapv)
        return()
    endif()
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<${PS2X_OPT_CONFIGS}:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
            # /GL (whole-program) stays Release-ONLY and paired with /LTCG below: turning LTCG on
            # for RelWithDebInfo would put 7,800 generated TUs through one link, and nobody here
            # has a Windows box to prove that does not exhaust the linker. The /O2 /Ob2 above is
            # where the speed actually is.
            $<$<CONFIG:Release>:/GL>
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    # GCC LTO is disabled: with 97 Unity TUs the linker spawns hundreds of
    # lto1 processes that exhaust system RAM (observed OOM on 16GB box).
    # -O3 already provides most of the speed; the real perf wins come from
    # disabling PS2X_TEXWATCH (32MB per-frame scan) and using ccache.
    if(MSVC AND IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()