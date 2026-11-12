# Arcradient (`MSHTML-Next`)

[![Arcradient CI & Cross-Platform Builds](https://github.com/NiSeullent/Arcradient/actions/workflows/ci.yml/badge.svg)](https://github.com/NiSeullent/Arcradient/actions/workflows/ci.yml)

구형 PC(**Windows 2000 SP4 · 32비트 `SSE1`**)부터 현대 64비트 시스템(**`SSE3` 최소 기준선 + 선택적 `AVX2`/GPU**)까지 단일 **순수 C (`C99`/`C11`)** 코드베이스로 묶는 경량 모듈형 웹 엔진 및 브라우저입니다.

- **통합 설계·개발 계획서:** [docs/design-plan.md](docs/design-plan.md)
- **공개 임베딩 C API:** [include/arcradient_embed.h](include/arcradient_embed.h)
- **동적 모듈 C ABI 규격:** [include/arcr_module.h](include/arcr_module.h)

---

## CPU 소프트웨어 렌더링 시제품 미리보기

| Modern Light (기본 압축형 UI) | Modern Dark (선택형 북마크 바 활성) | Windows 2000 Classic (`COLOR_3DFACE`) |
| :---: | :---: | :---: |
| ![Modern Light](docs/screenshots/shell_modern_light.png) | ![Modern Dark](docs/screenshots/shell_modern_dark.png) | ![Windows 2000 Classic](docs/screenshots/shell_win2000_classic.png) |

---

## 핵심 아키텍처 특징

1. **CPU 소프트웨어 기본 렌더링 & 비용 추정 기반 가속 (`arcr-accel`)**
   - CPU Scalar 기준 구현과 비트 단위로 일치하는 `SSE1 + 64-bit SWAR`(Pentium III 호환) 및 `SSE2/SSE3` XMM 커널 제공
   - GPU 업로드·동기화·회수 비용을 비교해 실제 이득이 있는 작업만 가속하며, 장치 손실 시 프로세스 종료 없이 CPU 경로로 즉시 복귀
2. **온디맨드 동적 모듈 적재 (`ArcrModuleDescriptor` ABI)**
   - 정적 HTML/CSS 문서 열람 시 `arcr_js_portable`, `arcr_media`, `arcr_devtools` 미적재(`0 Byte` JS 오버헤드)
   - 동적 라이브러리 적재 직전 아키텍처·최소 OS 버전·필수 SIMD 플래그를 검증해 구형 CPU의 `SIGILL`(`Illegal Instruction`) 원천 차단
3. **초기 단계부터 내장된 한국어 조판 및 입력 지원**
   - 한글 어절 단위 줄바꿈(`word-break: keep-all`), 행두·행말 문장 부호 금칙처리, Win32 `Imm32`(`WM_IME_COMPOSITION`) 조합 입력 및 굴림/맑은 고딕 자동 폴백
4. **엔진과 분리된 경량 브라우저 셸 (`arcr_shell`)**
   - `Modern Light`, `Modern Dark`, `Windows 2000 Classic`(불투명 `COLOR_3DFACE` + 3D 입체 테두리) 3종 테마 내장
   - Windows에서 `arcradient.exe` 실행 시 `CreateDIBSection` 기반 네이티브 Win32 GDI 브라우저 창 구동 (`F2`: 테마 전환, `Ctrl+B`: 선택형 북마크 바 토글, 탭 클릭 및 마우스 휠 스크롤 지원)

---

## 디렉터리 구조

```text
.
├── include/          # 공개 C API (arcradient_embed.h) 및 동적 모듈 ABI (arcr_module.h)
├── base/             # Arena/Slab 할당기, Atom 인터닝, UTF-8/한글 금칙처리, OSXSAVE 인식 CPUID
├── platform/         # OS 추상화 계층 (Win32 WINVER=0x0500 GDI/CryptGenRandom · POSIX FreeType/libpng)
├── accel/            # 비용 추정 라우터 + Scalar / SSE1+SWAR / SSE2 래스터라이저 + 무중단 CPU 복귀
├── core/             # HTML5 오류 복구 토크나이저/트리 빌더, DOM 트리, 엔진 수명주기 관리
├── css/              # CSS 선택자, 조상 블룸 필터, CSS 변수 var(), 형제 스타일 공유 캐시
├── layout/           # 블록·인라인 서식 맥락 및 한국어 keep-all 줄바꿈 엔진
├── paint/            # 표시 목록(Display List) 생성 및 더티 타일 CPU 래스터화
├── net/              # [온디맨드 모듈] URL 파서, 자체 루트 CA 검증 및 CMOS 시계 오차 감지
├── js/               # [온디맨드 모듈] ArcradientJS 호환 계층 (Portable C 실행기)
├── media/            # [온디맨드 모듈] Arcradient-Media 적응형 해상도·프레임 드롭 정책기
├── devtools/         # [온디맨드 모듈] 온디맨드 DOM/메모리 진단 덤프 모듈
├── shell/            # 경량 브라우저 셸 UI 렌더러 및 Win32 GDI 창 / 스냅샷 실행기
├── tests/            # 8대 핵심 보장 항목 검증 스위트 (CTest)
└── docs/             # 승인된 통합 설계·개발 계획서 (design-plan.md)
```

---

## 빌드 및 검증 실행

### 1. Linux / POSIX 네이티브 빌드

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 단위·통합 회귀 테스트 실행
ctest --test-dir build --output-on-failure -V

# 브라우저 셸 3종 테마 PNG 스냅샷 생성
./build/arcradient --module-dir ./build --snapshot-dir ./build
```

### 2. Windows 크로스 빌드 (MinGW-w64)

```bash
# Legacy x86 (Windows 2000 SP4 · PE 5.0 · Pentium III SSE1 · 외부 런타임 DLL 무의존)
cmake -B build-win2k -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/win2000-x86-sse1.cmake
cmake --build build-win2k

# Classic / Modern x64 (SSE3 Baseline · AVX 비필수)
cmake -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/win-x64-sse3.cmake
cmake --build build-win64
```
