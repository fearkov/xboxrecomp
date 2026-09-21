# ============================================================================
# Shin Megami Tensei: Nine (DDS9) - static recompilation target
#
# XBE facts, from game_files/default_analysis.json:
#   Title:       DDS9
#   Title ID:    0x41540002
#   Base addr:   0x00010000
#   Entry point: 0x0021AF2F
#   Image size:  7192608
# ============================================================================

# Generated translation units. CONFIGURE_DEPENDS so re-running the recompiler
# picks up added or removed chunks without a manual re-configure: a plain GLOB
# is evaluated once at configure time, and a newly generated chunk -- or
# recomp_stubs_unresolved.c, which is easy to forget -- would then be left out
# silently and surface as unresolved externals at link.
# RECOMP_GEN_DIR lets a second generation be built side by side with the one
# in place, so a lifter change can be measured against the binary it replaces
# instead of overwriting it.
set(RECOMP_GEN_DIR "recomp/gen" CACHE STRING "Directory holding generated chunks")
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/${RECOMP_GEN_DIR}/*.c")

# recomp_manual.c is not optional scaffolding: the generated code references
# recomp_lookup_manual, recomp_icall_fail_log and recomp_icall_not_code_log
# from every indirect call site. Leaving it out is 8497 unresolved externals
# across exactly three symbols.
add_executable(smt9_recomp WIN32
    main.c
    recomp_manual.c
    ${RECOMP_GEN_SOURCES}
)

target_link_libraries(smt9_recomp PRIVATE xboxrecomp)

target_include_directories(smt9_recomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/${RECOMP_GEN_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

# Windows SDK libraries the runtime layers call into. dbghelp lets the fault
# handler resolve a host RIP back to the generated symbol, i.e. to the guest
# function that faulted.
target_link_libraries(smt9_recomp PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    dbghelp
)

if(MSVC)
    target_compile_options(smt9_recomp PRIVATE /bigobj)
    target_link_options(smt9_recomp PRIVATE /LARGEADDRESSAWARE)
else()
    # COFF holds 32767 sections, and the generated chunks blow past that long
    # before they finish -- two of them are over 200 MB of C. GNU as needs
    # -mbig-obj to emit the extended object format; without it it stops with
    # "too many sections". This is the MinGW counterpart of MSVC's /bigobj.
    target_compile_options(smt9_recomp PRIVATE -Wa,-mbig-obj)
endif()

# Generated code is machine output: its warnings are not actionable, and at
# this volume they bury the ones from hand-written code. Silencing it here
# rather than project-wide keeps main.c and the runtime honest.
#
# Note the cost of this, though: it is exactly what hid the missing <math.h>
# in recomp_types.h, where implicit declarations made every lifted sqrt()
# return int. Warnings are off because they are unreadable, not because they
# are wrong -- when generated code misbehaves, build one chunk with them on.
if(NOT MSVC)
    set_source_files_properties(${RECOMP_GEN_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w")
endif()
