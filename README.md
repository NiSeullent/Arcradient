# Waterscape Browser 2.0 (`Arcradient` / `MSHTML-Next` Pure C11 Engine)

[![Waterscape CI & Cross-Platform Builds](https://github.com/NiSeullent/Arcradient/actions/workflows/ci.yml/badge.svg)](https://github.com/NiSeullent/Arcradient/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/screenshots/waterscape_256.png" width="112" alt="Waterscape Browser Icon" />
</p>

**Waterscape 2.0**은 구형 PC(**Windows 2000 SP4 · 32비트 `SSE1`**)부터 최신 멀티코어 64비트 시스템(**`SSE3` 기준선 + 런타임 `AVX2` 256-bit SIMD + 현대/레거시 하드웨어 GPU 가속**)까지 단일 **순수 C11(Zero-C++ / Zero-Chromium)** 아키텍처로 구동되는 초경량·초고속 2026년 모던 실사용 웹 브라우저입니다.

- **엔진 코드명:** `Arcradient` (`MSHTML-Next`) — 완전 클린룸 데이터 지향 Pure C11 엔진
- **설계·개발 계획서:** [docs/design-plan.md](docs/design-plan.md)
- **8계층 모듈 체계 규격:** [include/waterscape_modules.h](include/waterscape_modules.h)
- **공개 임베딩 C API:** [include/arcradient_embed.h](include/arcradient_embed.h)
- **동적 모듈·확장·플러그인 ABI:** [include/arcr_module.h](include/arcr_module.h) · [addons/ws_addons.h](addons/ws_addons.h)

---

## Waterscape 2.0 실제 렌더링 화면

| YouTube 2026 워치 시네마 & 60 FPS `<video>` | Zuzunza (주전자닷컴) Next.js + Tailwind v4 (`oklch` · `SVG`) |
| :---: | :---: |
| ![Waterscape YouTube Cinema](docs/screenshots/waterscape_youtube_cinema.png) | ![Waterscape Modern Sites](docs/screenshots/waterscape_modern_sites.png) |

| 새 탭 & Quick Dial (`Modern Dark`) | 2026 모던 웹 쇼케이스 (`Modern Light`) |
| :---: | :---: |
| ![Waterscape New Tab Dark](docs/screenshots/waterscape_newtab_dark.png) | ![Waterscape Showcase Light](docs/screenshots/waterscape_showcase_light.png) |

| 확장 기능(`.wsx`) · 하드웨어 가속 · F12 DevTools | Windows 2000 Classic (`COLOR_3DFACE` 입체 크롬) |
| :---: | :---: |
| ![Waterscape Addons & Hardware](docs/screenshots/waterscape_addons_hardware.png) | ![Waterscape Win2000 Classic](docs/screenshots/waterscape_win2000_classic.png) |

---

## 벤치마크 및 하드웨어 활용 성능 (Waterscape 2.0 실측치)

| 측정 항목 | 측정 환경 / 경로 | 실측 성능 |
| :--- | :--- | :--- |
| **1280×800 전체 브라우저 프레임 렌더링** | Y-Band Spatial Binning + Multi-Core `64×64` Tile + `AVX2` + Lock-Free Glyph Atlas | **1,341.6 FPS** (`0.745 ms/frame`, `1,373.8 MPix/s`) |
| **캐시 탭 전환 + `1080×720` 전체 합성** | 탭별 전용 프레임버퍼 캐시 (`cached_fb`) 즉시 블릿 | **0.62 ms** (`622 µs` — 탭 전환 렉 **0**) |
| **SIMD 알파 합성 커널 처리량** | `AVX2 256-bit` (8픽셀 동시 처리) vs `Scalar` 기준 구현 | **5,237 MPix/s** vs `635 MPix/s` (**8.25× 가속**, 100% 비트 일치) |
| **메모리 안정성 & 패키지 용량** | `ASan + LSan + UBSan` 전 33개 스위트 `0` 누수 · 전체 인스톨러 | **~1.3 MB** (`.deb`) / **~1.5 MB** (`.run` SFX) |

---

## Waterscape 2.0 핵심 기술 특징

### 1. 2026년 최신 웹 규격 전면 지원 & 주요 모던 사이트 완벽 대응
- **QuickJS-NG ES2026 자바스크립트 엔진 + 모던 Web API (`arcr_js_portable`):**
  - `Promise` 마이크로태스크 큐, `async`/`await`, 옵셔널 체이닝(`?.`), 널 병합(`??`), 정규표현식, 클래스/모듈 문법 완비
  - `fetch()`, `XMLHttpRequest`, `URL` / `URLSearchParams`, `Event` / `CustomEvent` (`preventDefault()` SPA 라우터 호환), `setTimeout` / `requestAnimationFrame`, `MutationObserver` / `ResizeObserver` / `IntersectionObserver`, `customElements`, `localStorage` / `sessionStorage`, `CanvasRenderingContext2D`, `HTMLMediaElement` 및 `1,500ms` 무한 루프 인터럽트 가드 내장
- **차세대 압축·이미지·벡터 코덱 (`arcr_codec`):**
  - HTTP `Brotli (br)` / 멀티 멤버 `gzip` / `deflate` 압축 해제
  - `WebP` (`VP8`/`VP8L`/알파), `JPEG`, `PNG`, 애니메이션 `GIF`, `BMP`, `ICO` 및 인라인 `<svg>` / `data:image/svg+xml` 벡터 래스터화(`currentColor` 틴팅 포함), sRGB 감마 보존 고품질 리샘플러 내장
- **Tailwind CSS v4 & 60 FPS 애니메이션 (`arcr_css`, `arcr_layout`, `arcr_paint`):**
  - `@layer`, `@supports`, `@media (width >= ...)`, 이스케이프 클래스(`.sm\:flex`, `.bg-\[\#1e293b\]`), 요소별 `--*` 커스텀 프로퍼티 상속, `oklch()` / `oklab()` / `color-mix()`, `linear-gradient()`, `position: relative/absolute/fixed/sticky` + `z-index` 스태킹 컨텍스트, `overflow` 클리핑, `aspect-ratio`, 2D `transform`, 60 FPS `@keyframes` 애니메이션 보간 및 $O(N)$ 중첩 Flexbox 메모이제이션
- **모던 사이트 스마트 하이드레이터 & HTML5 `<video>` 시네마 디코더 (`arcr_media`):**
  - **YouTube** (`ytInitialPlayerResponse` + `ytInitialData` 워치 시네마 & 홈 그리드), **Zuzunza (`www.zuzunza.com`)** 등 Next.js App Router/React 19 SSR 복원 보호기, **Google 검색** (`<noscript>` 방어 + 단일 행 `<textarea name="q">` 엔터 검색), **X (Twitter)** 공개 신디케이션 뷰어 지원
  - POSIX `ffmpeg` 파이프 + Windows 동적 Media Foundation(`mfplat.dll`/`mfreadwrite.dll`) + 60 FPS Pure C 시네마 폴백 내장

### 2. 선명한 256단계 sRGB 폰트 안티에일리어싱 & 무지연 멀티코어 UI
- **256단계 지각적 sRGB 스템 다크닝 & 감마 보정 안티에일리어싱:**
  - Windows `ClearType` / `ANTIALIASED_QUALITY` (`GGO_GRAY8_BITMAP` 65→256 감마 LUT + 써로게이트 이모지 `cmap` 포맷 12 지원) 및 Linux `FreeType FT_LOAD_TARGET_LIGHT` + `stb_truetype` 내장 폴백으로 다크·라이트 배경 모두에서 깨짐 없는 선명한 한글/영문 가독성 구현
- **락프리 글리프 아틀라스 + 무지연 탭 전환 & 비동기 멀티코어 구조:**
  - 메인 스레드 글리프 프리워밍 후 멀티코어 타일 워커가 **뮤텍스 락·메모리 복사 없이** 아틀라스 포인터를 직접 읽는 락프리 구조
  - 탭별 프레임버퍼 캐시(`0.62 ms` 즉시 전환), 백그라운드 비동기 페이지 네비게이션 + 애니메이션 로딩 바, 멀티코어 병렬 서브리소스 커넥션 풀(8-슬롯 Keep-Alive TLS 1.3), 우클릭 컨텍스트 메뉴, `Ctrl+Shift+T`, `view-source:`(`Ctrl+U`), `F11` 전체화면 지원

### 3. 최신 + 구버전 경로 그래픽/하드웨어 가속 및 완전한 설치 프로그램
- **GPU & SIMD 전 세대 가속:** Windows `D3D11 + DXGI Flip` → `Vulkan 1.x` → `D3D9 Hardware HAL` → `OpenGL Textured Quad` → `GDI DIBSection`, Linux `X11 MIT-SHM Zero-Copy` + 런타임 `AVX2 (256-bit)` / `SSE2·SSE3` / `SSE1+SWAR` 디스패치
- **공식 인스톨러 & 언인스톨러:**
  - Windows: `WaterscapeSetup-2.0.0-x64.exe` / `WaterscapeSetup-2.0.0-win2000-x86.exe` (`PE 5.0`, 외부 런타임 DLL `0`개, `uninstall.exe` 포함)
  - Linux: `waterscape_2.0.0_amd64.deb` / `Waterscape-2.0.0-linux-x86_64.run` (`SHA-256` 무결성 검증 + `uninstall.sh`)

---

## 빌드, 벤치마크 및 설치 패키지 생성

### 1. Linux 네이티브 빌드 및 인스톨러 패키징

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 33개 전 계층 단위·통합·모던 웹 검증 스위트 실행
ctest --test-dir build --output-on-failure -V

# 멀티코어 + AVX2 렌더링 벤치마크 실행
./build/waterscape --module-dir ./build --benchmark

# Debian (.deb) 및 범용 자가 설치 (.run) 인스톨러 생성
mkdir -p dist
bash installer/linux/build_deb.sh ./build dist/waterscape_2.0.0_amd64.deb
bash installer/linux/build_run_installer.sh ./build dist/Waterscape-2.0.0-linux-x86_64.run
```

### 2. Windows 크로스 빌드 및 단일 파일 `WaterscapeSetup.exe` 생성 (MinGW-w64)

```bash
# Windows 2000 SP4 ~ Windows 11 호환 32비트 (PE 5.0 · Pentium III SSE1 · 외부 런타임 DLL 0개)
cmake -B build-win2k -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/win2000-x86-sse1.cmake
cmake --build build-win2k

# Classic / Modern 64비트 (SSE3 기준선 + 런타임 AVX2 & D3D11/Vulkan/D3D9 가속)
cmake -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/win-x64-sse3.cmake
cmake --build build-win64
```
