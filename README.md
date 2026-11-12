# Waterscape Browser 1.0 (`Arcradient` / `MSHTML-Next` Engine)

[![Waterscape CI & Cross-Platform Builds](https://github.com/NiSeullent/Arcradient/actions/workflows/ci.yml/badge.svg)](https://github.com/NiSeullent/Arcradient/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/screenshots/waterscape_256.png" width="112" alt="Waterscape Browser Icon" />
</p>

**Waterscape**는 구형 PC(**Windows 2000 SP4 · 32비트 `SSE1`**)부터 최신 멀티코어 64비트 시스템(**`SSE3` 기준선 + 런타임 `AVX2` 256-bit SIMD + 현대/레거시 하드웨어 GPU 가속**)까지 단일 **순수 C11(Zero-C++ / Zero-Chromium)** 코드베이스로 구동되는 초경량 실사용 웹 브라우저입니다.

- **엔진 코드명:** `Arcradient` (`MSHTML-Next`) — 완전 클린룸 데이터 지향 재구현
- **설계·개발 계획서:** [docs/design-plan.md](docs/design-plan.md)
- **8계층 모듈 체계 규격:** [include/waterscape_modules.h](include/waterscape_modules.h)
- **공개 임베딩 C API:** [include/arcradient_embed.h](include/arcradient_embed.h)
- **동적 모듈·확장·플러그인 ABI:** [include/arcr_module.h](include/arcr_module.h) · [addons/ws_addons.h](addons/ws_addons.h)

---

## Waterscape 1.0 실제 렌더링 화면

| 새 탭 & 사생활 보호 (`Modern Dark`) | CSS3 Grid/Flex/Form/Canvas 쇼케이스 (`Modern Light`) |
| :---: | :---: |
| ![Waterscape New Tab Dark](docs/screenshots/waterscape_newtab_dark.png) | ![Waterscape Showcase Light](docs/screenshots/waterscape_showcase_light.png) |

| 확장 기능(`.wsx`) · 하드웨어 가속 · F12 DevTools | Windows 2000 Classic (`COLOR_3DFACE` 입체 크롬) |
| :---: | :---: |
| ![Waterscape Addons & Hardware](docs/screenshots/waterscape_addons_hardware.png) | ![Waterscape Win2000 Classic](docs/screenshots/waterscape_win2000_classic.png) |

---

## 벤치마크 및 하드웨어 활용 성능 (실측치)

| 측정 항목 | 측정 환경 / 경로 | 실측 성능 |
| :--- | :--- | :--- |
| **1280×800 전체 브라우저 프레임 렌더링** | Multi-Core `64×64` Tile Work-Stealing + `AVX2` + O(1) Glyph Atlas | **526.2 FPS** (`1.900 ms/frame`, `538.9 MPix/s`) |
| **SIMD 알파 합성 커널 처리량** | `AVX2 256-bit` (8픽셀 동시 처리) vs `Scalar` 기준 구현 | **5,224 MPix/s** vs `633 MPix/s` (**8.25× 가속**, 100% 비트 일치) |
| **아레나 메모리 사용량** | 복합 탭 문서 파싱 + CSS3 계산 + 레이아웃 + 디스플레이 리스트 | **< 310 KiB** (탭당 O(1) Arena Reset, 메모리 단편화 `0`) |
| **패키지·설치 용량** | 전체 브라우저 + 엔진 + 확장 3종 + 네이티브 플러그인 + 언인스톨러 | **~317 KiB** (`.deb`) / **~527 KiB** (`.run` SFX) |

---

## 핵심 기술 특징

### 1. 최신 경로 + 구버전 경로 그래픽/하드웨어 가속 전면 지원
- **현대 GPU 경로:**
  - Windows 8/10/11: **Direct3D 11 + DXGI Flip-Model SwapChain** (`d3d11.dll` / `dxgi.dll` 런타임 동적 바인딩)
  - Windows / Linux 공통: **Vulkan 1.x** 물리 장치 프로브 (`vulkan-1.dll` / `libvulkan.so.1`)
  - Linux X11 데스크톱: **X11 MIT-SHM Zero-Copy 공유 메모리 프레젠터** (`XShmPutImage`)
- **레거시 GPU 경로:**
  - Windows 2000 / XP / Vista / 7: **Direct3D 9.0c / D3D9Ex** 하드웨어 텍스처 스왑체인 (`d3d9.dll`), **DirectDraw 7** (`ddraw.dll`), **OpenGL ICD** (`opengl32.dll`, `"GDI Generic"` 및 `"llvmpipe"` 소프트웨어 에뮬레이터 자동 배제)
  - 장치 손실(`DXGI_ERROR_DEVICE_REMOVED` / `D3DERR_DEVICELOST`) 발생 시 프로세스 중단 없이 즉시 **Win32 GDI `CreateDIBSection`** / CPU 타일 경로로 무중단 복귀
- **CPU 멀티코어 & 전 세대 SIMD 디스패치:**
  - 런타임 `CPUID` + `XGETBV`(`OSXSAVE`) 검증으로 **`AVX2 (256-bit)` → `SSE2/SSE3 (128-bit)` → `SSE1+SWAR (Pentium III)` → `Scalar`** 자동 선택
  - 원자적 작업 훔치기(`Atomic Work-Stealing`) 스레드 풀 기반 **`64×64` 병렬 타일 래스터라이저** 및 스레드 안전 **4-way 2048-슬롯 O(1) 글리프 아틀라스 캐시**

### 2. 클린룸 최신 웹 규격 자체 재구현 (Zero-Chromium)
- **HTML5 & DOM:** 자동 태그 복구 파서, 시맨틱 요소, 테이블, 폼 컨트롤, 인라인 SVG/PNG/BMP/`data:` URI 이미지, `querySelector`/`querySelectorAll`, `innerHTML` 동적 프래그먼트 파싱
- **O(1) CSS3 엔진 (`arcr_css`):** 우측 키 해시 버킷(`64-way`) + `64-bit` 조상 블룸 필터 + 형제 스타일 공유 캐시, **CSS Flexbox**, **CSS Grid**, **HTML Table**, `var()`, `calc()`, `@media (max-width / min-width / prefers-color-scheme)`, `:hover`/`:focus`/`:first-child`/`:last-child`, `border-radius`, `box-shadow`
- **바이트코드 VM 기반 `ArcradientJS` (`arcr_js_portable`):** 클로저, 화살표 함수, 배열/객체 리터럴, `JSON`, `Math`, 실시간 DOM 조작·이벤트 버블링, `<canvas>` 2D 드로잉 컨텍스트, 무한 루프 방지 연료 가드(`500,000 ops`)
- **실시간 HTTPS 네트워크 (`arcr_net`):** RFC 3986 상대 URL 해석, HTTP/1.1 Keep-Alive/리다이렉트/Chunked 디코딩, POSIX **OpenSSL** & Windows **SChannel (`secur32.dll`, Win2000~Win11 내장 TLS)**, 자체 `gzip`/`deflate` 압축 해제, 쿠키 저장소, 32-엔트리 LRU 캐시

### 3. 확장 기능(`.wsx`) 및 네이티브 C 플러그인 아키텍처
- **WebExtensions 호환 `.wsx` 패키지:** 자체 PKZIP/DEFLATE 코덱으로 패킹·검증되는 확장 아카이브 규격
  - `waterscape-shield.wsx`: FNV-1a O(1) 도메인 해시 기반 내장 광고·추적기 차단기
  - `waterscape-dark.wsx`: 스마트 다크 리더 스타일시트 주입 확장
  - `waterscape-reader.wsx`: 본문 집중 읽기 모드 확장
- **네이티브 C11 플러그인 ABI (`WaterscapePluginVTable`):**
  - `ws_plugin_perf_hud` (`.dll` / `.so`): 실시간 프레임 시간·타일·SIMD·아레나 메모리 텔레메트리 HUD 오버레이 플러그인

### 4. 완전한 네이티브 설치 프로그램 및 언인스톨러 내장
- **Windows (`WaterscapeSetup-1.0.0-x64.exe` / `WaterscapeSetup-1.0.0-win2000-x86.exe`):**
  - 단일 실행 파일 자가 압축 해제(Pure C11 SFX + Win32 GUI 설치 마법사 & `--silent` 무인 설치), 시작 메뉴/바탕화면 바로가기(`IShellLink`), `Uninstall` 레지스트리 및 `RegisteredApplications` 등록, 전용 `uninstall.exe` 및 NSIS 스크립트(`installer/win32/waterscape.nsi`) 포함
- **Linux (`waterscape_1.0.0_amd64.deb` / `Waterscape-1.0.0-linux-x86_64.run`):**
  - 표준 Debian/Ubuntu `.deb` 패키지 및 모든 배포판에서 동작하는 SHA-256 무결성 검증 자가 설치 `.run` 인스톨러 + `uninstall.sh` 제공

---

## 8계층 모듈 체계 (`include/waterscape_modules.h`)

```text
Tier 7: Installer & Packaging   ── installer/{win32,linux}/, tools/{gen_brand_assets,pack_installer}.c
Tier 6: Waterscape Desktop App  ── shell/{arcr_shell,waterscape_win32,waterscape_x11,main}.c
Tier 5: Add-ons & Plugin Host   ── addons/ws_addons.c, addons/bundled/*, plugins/perf_hud/, tools/pack_wsx.c
Tier 4: On-Demand Dynamic Slots ── net/arcr_net.c, js/arcr_js_portable.c, media/arcr_media.c, devtools/arcr_devtools.c
Tier 3: Clean-Room Web Core     ── core/{arcr_dom,arcr_html_parser,arcr_engine}.c, css/arcr_css.c, layout/arcr_layout.c, paint/arcr_paint.c
Tier 2: Hardware & SIMD Accel   ── accel/{arcr_accel,arcr_accel_avx2}.c (AVX2/SSE2/SSE1+SWAR + D3D11/Vulkan/D3D9/DDraw7/GL)
Tier 1: OS Platform PAL         ── platform/win32/arcr_platform_win32.c, platform/posix/arcr_platform_posix.c (O(1) Glyph Atlas)
Tier 0: Zero-Dep Foundation     ── base/{arcr_arena,arcr_atom,arcr_str,arcr_utf8,arcr_cpuid,arcr_codec,arcr_threadpool}.c
```

---

## 빌드, 벤치마크 및 설치 패키지 생성

### 1. Linux 네이티브 빌드 및 인스톨러 패키징

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 22개 전 계층 단위·통합 검증 스위트 실행
ctest --test-dir build --output-on-failure -V

# 멀티코어 + AVX2 렌더링 벤치마크 실행
./build/waterscape --module-dir ./build --benchmark

# Debian (.deb) 및 범용 자가 설치 (.run) 인스톨러 생성
mkdir -p dist
bash installer/linux/build_deb.sh ./build dist/waterscape_1.0.0_amd64.deb
bash installer/linux/build_run_installer.sh ./build dist/Waterscape-1.0.0-linux-x86_64.run
```

### 2. Windows 크로스 빌드 및 단일 파일 `WaterscapeSetup.exe` 생성 (MinGW-w64)

```bash
# Windows 2000 SP4 ~ Windows 11 호환 32비트 (PE 5.0 · Pentium III SSE1 · 외부 런타임 DLL 0개)
cmake -B build-win2k -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/win2000-x86-sse1.cmake
cmake --build build-win2k
# 결과물: build-win2k/WaterscapeSetup.exe (자가 설치 마법사 + waterscape.exe + uninstall.exe + .wsx 확장 내장)

# Classic / Modern 64비트 (SSE3 기준선 + 런타임 AVX2 & D3D11/Vulkan/D3D9 가속)
cmake -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/win-x64-sse3.cmake
cmake --build build-win64
# 결과물: build-win64/WaterscapeSetup.exe
```
