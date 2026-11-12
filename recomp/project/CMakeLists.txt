cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)   # CMAKE_MSVC_RUNTIME_LIBRARY (CRT estatica na build de distribuicao)
project(wdc_recomp C CXX RC)

# WDC_RELEASE=ON: build de distribuicao (Leo Build): CRT estatica (/MT: o usuario nao precisa instalar o Visual C++ Redistributable), sem o caminho de
# desenvolvimento da ROM embutido e sem o interpretador de RSP de referencia (diagnostico). Veja tools/release/.
option(WDC_RELEASE "Build de distribuicao (CRT estatica, sem caminhos de desenvolvimento)" OFF)
if (WDC_RELEASE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(WDC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(RT "${WDC_ROOT}/thirdparty/N64ModernRuntime")

# WDC_TRACE=ON usa o C gerado com trace_mode (RecompiledFuncsTrace/): mapa de cobertura de funcoes.
option(WDC_TRACE "Usar o C gerado com trace_mode (cobertura de funcoes)" OFF)
# WDC_GEN_SUBDIR=RecompiledFuncs60: usa o C gerado por `gen_wdc_toml.py --exp60` (experimento de 60 fps) em vez do normal.
set(WDC_GEN_SUBDIR "" CACHE STRING "Subpasta de recomp/ com o C gerado (vazio = padrao)")
if (WDC_GEN_SUBDIR)
    set(GEN "${WDC_ROOT}/recomp/${WDC_GEN_SUBDIR}")
elseif (WDC_TRACE)
    set(GEN "${WDC_ROOT}/recomp/RecompiledFuncsTrace")
else()
    set(GEN "${WDC_ROOT}/recomp/RecompiledFuncs")
endif()

# Avisos "unsafe" do CRT da Microsoft (getenv/fopen/sscanf marcados como deprecated): sao funcoes padrao do C e, no nosso codigo,
# todo uso checa o retorno. Vale para o projeto e para as dependencias (parallel-rdp usa getenv/fopen do mesmo jeito).
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# ultramodern + librecomp (compilados junto, com o mesmo compilador/flags deste projeto).
add_subdirectory("${RT}" "${CMAKE_BINARY_DIR}/runtime")

# Avisos em codigo de terceiros que nao alteramos (o thirdparty fica intacto), silenciados so no alvo de origem:
#  - librecomp: rabbitizer (Utils.h) redefine CONST/UNUSED, que o windows.h ja define (-Wmacro-redefined); o resultado e o mesmo.
#  - fmt: o construtor depreciado de locale_ref chama std::isalpha so para checar ADL e descarta o retorno (-Wunused-result).
#  - ultramodern: osDestroyThread (extern "C") lanca ultramodern::thread_terminated para encerrar a propria thread (projeto do upstream).
#    O /EHsc do clang-cl assume que funcoes extern "C" nunca lancam (-fexternc-nounwind); /EHc- remove essa suposicao, que e falsa aqui.
if (TARGET librecomp)
    target_compile_options(librecomp PRIVATE -Wno-macro-redefined)
endif()
if (TARGET fmt)
    target_compile_options(fmt PRIVATE -Wno-unused-result)
endif()
option(WDC_EXTERNC_MAY_THROW "ultramodern: /EHc- (funcoes extern \"C\" podem lancar; ver acima)" ON)
if (TARGET ultramodern AND WDC_EXTERNC_MAY_THROW)
    target_compile_options(ultramodern PRIVATE /EHc-)
endif()

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN}/funcs_*.c")


# ---------------------------------------------------------------------------------------------------------------------------------
# ParaLLEl-RDP (Themaister, MIT; thirdparty/parallel-rdp-standalone): renderizador de RDP em Vulkan compute, bit-exato com o angrylion.
# Lista de fontes = config.mk do proprio repositorio standalone (shaders ja embutidos em parallel-rdp/shaders/slangmosh.hpp).
# ---------------------------------------------------------------------------------------------------------------------------------
option(WDC_RDP_VK "Renderizar o RDP com ParaLLEl-RDP (Vulkan)" ON)
if (WDC_RDP_VK)
    set(PRDP "${WDC_ROOT}/thirdparty/parallel-rdp-standalone")
    file(GLOB PRDP_RDP_SRCS "${PRDP}/parallel-rdp/*.cpp")
    set(PRDP_VK_NAMES buffer buffer_pool command_buffer command_pool context cookie descriptor_set device event_manager fence
        fence_manager image indirect_layout memory_allocator pipeline_event query_pool render_pass sampler semaphore
        semaphore_manager shader texture/texture_format)
    set(PRDP_UTIL_NAMES arena_allocator logging thread_id aligned_alloc timer timeline_trace_file environment thread_name)
    set(PRDP_SRCS ${PRDP_RDP_SRCS})
    foreach(n ${PRDP_VK_NAMES})
        list(APPEND PRDP_SRCS "${PRDP}/vulkan/${n}.cpp")
    endforeach()
    foreach(n ${PRDP_UTIL_NAMES})
        list(APPEND PRDP_SRCS "${PRDP}/util/${n}.cpp")
    endforeach()
    list(APPEND PRDP_SRCS "${PRDP}/volk/volk.c")
    add_library(parallel_rdp STATIC ${PRDP_SRCS})
    target_include_directories(parallel_rdp PUBLIC
        "${PRDP}/parallel-rdp" "${PRDP}/volk" "${PRDP}/vulkan" "${PRDP}/vulkan-headers/include" "${PRDP}/util")
    # NOMINMAX/WIN32_LEAN_AND_MEAN com valor VAZIO (`NOMINMAX=`): os fontes do parallel-rdp fazem `#define NOMINMAX` (vazio) e o valor 1 da
    # linha de comando daria -Wmacro-redefined em cada arquivo.
    target_compile_definitions(parallel_rdp PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX= WIN32_LEAN_AND_MEAN=)
    set_target_properties(parallel_rdp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(parallel_rdp PUBLIC winmm)
endif()

add_executable(wdc_recomp
    src/main.cpp
    src/wdc_rsp_hw.cpp
    src/wdc_audio.cpp
    src/wdc_pfs.cpp
    src/wdc_pak.cpp
    src/wdc_window.cpp
    src/wdc_input.cpp
    src/wdc_config.cpp
    src/wdc_present.cpp
    src/wdc_setup.cpp
    src/wdc_rom.cpp
    src/wdc_saves.cpp
    src/wdc_joy.cpp
    src/wdc.rc
    "${WDC_ROOT}/recomp/rsp/RecompiledUcode/wdc_ucode.cpp"   # ucode grafico recompilado (RSPRecomp patchado; ver recomp/rsp/wdc_ucode.toml)
    "${WDC_ROOT}/recomp/runtime_glue/wdc_runtime_stubs.cpp"
    "${GEN}/lookup.cpp"
    ${GEN_SOURCES}
)

target_include_directories(wdc_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"         # trace.h
    "${GEN}"                                  # funcs.h, recomp_overlays.inl
    "${WDC_ROOT}/recomp/runtime_shim"         # recomp_shim.h (MEM_WU + declaracoes faltantes)
    "${RT}/N64Recomp/include"                 # recomp.h (identico ao do gerador; ABI do recomp_context)
)

target_compile_definitions(wdc_recomp PRIVATE
    NOMINMAX= WIN32_LEAN_AND_MEAN=   # vazios, como no parallel_rdp (os cabecalhos do runtime tambem fazem `#define WIN32_LEAN_AND_MEAN`)
)
if (WDC_RELEASE)
    # Sem janela de console (clique duplo no exe); o log vai para <config>/wdc_log.txt (ver main.cpp). O ponto de entrada continua sendo main().
    target_compile_definitions(wdc_recomp PRIVATE WDC_RELEASE_BUILD=1)
    target_link_options(wdc_recomp PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
else()
    target_compile_definitions(wdc_recomp PRIVATE "WDC_DEFAULT_ROM=\"${WDC_ROOT}/rom/wdc_bigendian.z64\"")
endif()

# O C gerado nao inclui MEM_WU nem declara as funcoes *_recomp que o runtime fornece: forca o shim em todos os arquivos.
if (MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(wdc_recomp PRIVATE "/FIrecomp_shim.h")
else()
    target_compile_options(wdc_recomp PRIVATE "-include" "recomp_shim.h")
endif()

# Hooks de libultra nos arquivos do C gerado (nao em main.cpp, glue e trace_hooks.cpp, que chamam as funcoes reais):
#  - wdc_hooks.h  (todas as builds): captura OS_EVENT_SP_BREAK, que o ultramodern ignora (ver wdc_runtime_stubs.cpp).
#  - trace_hooks.h (build trace, inclui wdc_hooks.h): alem disso, wrappers das trocas de mensagens (quem esta
#    bloqueado em qual fila) e pilha de chamadas por thread.
if (WDC_TRACE)
    target_sources(wdc_recomp PRIVATE src/trace_hooks.cpp src/trace_seg.cpp src/wdc_seg_wrappers.cpp)   # segmentos: tools/recomp/gen_seg_hooks.py
    target_compile_definitions(wdc_recomp PRIVATE WDC_TRACE_BUILD=1)
    # Simbolos de depuracao: o tratador de queda resolve RIP -> arquivo:linha do C gerado (cada linha tem o comentario `// 0x...: instr`).
    target_compile_options(wdc_recomp PRIVATE /Z7)
    target_link_options(wdc_recomp PRIVATE /DEBUG)
    target_link_libraries(wdc_recomp PRIVATE dbghelp)
    set(WDC_FORCE_HOOKS "/FItrace_hooks.h")
else()
    set(WDC_FORCE_HOOKS "/FIwdc_hooks.h")
endif()
set_source_files_properties("${GEN}/lookup.cpp" PROPERTIES COMPILE_OPTIONS "${WDC_FORCE_HOOKS}")
# O C gerado pelo N64Recomp passa `&lo, &hi` (uint64_t*) a DMULT/DDIV, declarados com int64_t* em recomp.h (-Wpointer-sign, 34 ocorrencias).
# E bem definido: int64_t e uint64_t sao as variantes com e sem sinal do mesmo tipo (C11 6.5p7) e DMULT/DDIV gravam o complemento de dois.
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${WDC_FORCE_HOOKS};-Wno-pointer-sign")

# O VU do RSP (librecomp/rsp_vu_impl.hpp) usa intrinsecos SSE4.1/SSSE3; o clang-cl exige a flag (o MSVC nao).
set_source_files_properties("${WDC_ROOT}/recomp/rsp/RecompiledUcode/wdc_ucode.cpp" PROPERTIES COMPILE_OPTIONS "/clang:-msse4.1;/clang:-mssse3")

# Interpretador de RSP de referencia (MAME, BSD-3; tools/rspref): motor alternativo do ucode para diagnostico (WDC_RSP_ENGINE=ref).
option(WDC_RSP_REF "Incluir o interpretador de RSP de referencia (MAME)" ON)
if (WDC_RELEASE)
    set(WDC_RSP_REF OFF)
endif()
if (WDC_RSP_REF)
    target_sources(wdc_recomp PRIVATE "${WDC_ROOT}/tools/rspref/rsp_ref.cpp")
    target_include_directories(wdc_recomp PRIVATE "${WDC_ROOT}/tools/rspref")
    target_compile_definitions(wdc_recomp PRIVATE WDC_RSP_REF=1 "WDC_UCODE_TEXT_BIN=\"${WDC_ROOT}/recomp/rsp/wdc_ucode_text.bin\"")
endif()

if (WDC_RDP_VK)
    target_sources(wdc_recomp PRIVATE src/wdc_rdp_vk.cpp)
    target_compile_definitions(wdc_recomp PRIVATE WDC_RDP_VK=1)
    target_link_libraries(wdc_recomp PRIVATE parallel_rdp)
endif()

target_link_libraries(wdc_recomp PRIVATE librecomp ultramodern ole32 user32 gdi32 d3d11 dxgi d3dcompiler gdiplus shlwapi comdlg32 shell32 uuid avrt dinput8 dxguid)   # ole32: WASAPI (wdc_audio.cpp); user32/gdi32: janela (wdc_window.cpp); d3d11/dxgi/d3dcompiler: apresentador em GPU (wdc_present.cpp)
