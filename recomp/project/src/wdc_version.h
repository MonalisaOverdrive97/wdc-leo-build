// Identificacao da build distribuida (aparece no setup, no titulo da janela, no --version e no LEIA-ME do pacote).
#pragma once

#define WDC_BUILD_NAME "Leo Build"
#define WDC_BUILD_CHANNEL "canary"
#define WDC_BUILD_VERSION "0.1.2"
#define WDC_BUILD_LABEL WDC_BUILD_NAME " " WDC_BUILD_VERSION " (" WDC_BUILD_CHANNEL ")"

// Versoes largas (L"...") para a interface em GDI+.
#define WDC_WIDEN2(x) L##x
#define WDC_WIDEN(x) WDC_WIDEN2(x)
#define WDC_BUILD_NAME_W WDC_WIDEN(WDC_BUILD_NAME)
#define WDC_BUILD_VERSION_W WDC_WIDEN(WDC_BUILD_VERSION)
#define WDC_BUILD_CHANNEL_W WDC_WIDEN(WDC_BUILD_CHANNEL)
