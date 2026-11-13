# Arcradient / Waterscape 2.0 — 2026년 웹 표준·클린룸 JS·단일 프레임 UI 최종 검증 보고서

- **검증 기준일**: `2026-11-13` (KST)
- **대상 저장소**: `NiSeullent/Arcradient`
- **초기 기준선 커밋 (`P0 Baseline SHA`)**: `7e51944af4a7fbfa5cc2af13e776fa10afd9fcc2` ([p0_baseline_evidence.md](p0_baseline_evidence.md))
- **검증 완료 코드 커밋 (`Verified Code SHA`)**: `eb6dcf8ac5189cdebf8345e1089d4ad1a1df9304` (GitHub Actions Run `43415059917` — 4개 잡 전원 녹색 통과)
- **검증 호스트 환경 (Linux x64 & GitHub Actions Windows Native)**:
  - OS / 커널: `Debian GNU/Linux 12 (bookworm)` · `Linux 6.12.105+ x86_64` 및 GitHub Actions `windows-latest` (`x86` WoW64 + `x64` 네이티브 GUI/UIA)
  - CPU / SIMD: `Intel(R) Xeon(R) Platinum 8481C` (`2`~`4` vCPU, `SSE1`..`SSE4.2`, `AVX`, `AVX2`)
  - GPU 프로브: `GPU-Vulkan-1.x` + `X11 MIT-SHM Zero-Copy` / Win32 `D3D11 + DWM Extended` + CPU SIMD 타일 폴백
  - 툴체인: `GCC 12.2.0` / `MinGW-w64 i686 & x86_64` (`-std=c11 -Wall -Wextra -Wpedantic -Wvla`), `CMake 3.25.1`, `Ninja 1.11.1`, `Python 3.11`
  - 테스트 실행 조건: 임시 사생활 보호 프로필(`--private`), 순수 표준 적합성 모드(`--conformance`, `core/arcr_site_hydrators.h` 사이트 이름 치환 배제), 기본 `96 DPI` 및 `120/144/192 DPI` 배율 왕복 검증, 기본 `.wsx` 애드온 4종 적재 일관성 확인

---

## 1. P0 기준선 결함 대비 P1~P4 근본 원인 해결 요약

| 항목 | P0 기준선 상태 (`7e51944`) | 근본 원인 | P1~P4 해결 결과 |
| :--- | :--- | :--- | :--- |
| **배포 JS 엔진 순수성** | `libarcr_js_portable.so`가 `third_party/quickjs/*` 직접 링크 (`JS_NewRuntime` 포함) | 자체 C11 엔진 부재 및 호스트 결합 | `js/vm/*`, `js/builtins/*`, `js/modules/ajs_module.c` 클린룸 순수 C11 `ArcradientJS` VM/GC/ESM 및 `web/bindings/*` 분리 완수 (`nm -D` 외부 JS 심볼 **0개**) |
| **외부 ESM + `importmap` (`S02`)** | `SyntaxError: Unexpected token '{'` (`title=before-esm`) | `<script type="importmap">`/`module` 미분리, 모듈 그래프 미구현 | `importmap` 해석 + 모듈 그래프 병렬 fetch → link → evaluate 분리, 순환 live binding, TLA, 동적 `import()` 통과 (**6/6 PASS**) |
| **비차단 서브리소스 & 취소 (`N03`)** | `700ms` 느린 이미지가 첫 표시를 `707.9ms` 동기 차단 | `load_html_internal` 동기 일괄 다운로드 | `AsyncSubresBatchJob` 비동기 스레드풀 전환: 첫 페인트 **`34.5ms`**, 조기 클릭 즉시 반응, 탐색 취소 **`1.85ms`** (`<= 50ms` 예산 충족) |
| **중립 호스트 SPA Hydration (`A01`)** | `127.0.0.1`에서 `title=A01-SSR:home` 정지 | 사이트 이름 하이드레이터 외 일반 ESM/History 미동작 | `--conformance` 모드에서 SSR → ESM hydration → 클릭/폼 → `fetch()` → `pushState`/`back`/`popstate` 왕복 완료 |
| **CSS 2026 (`C01`)** | `@supports` 거짓 양성, `col1:1080`, `@layer`/`@container`/`@scope` 미적용 | 조건부 규칙·캐스케이드 계층·다중 트랙 Grid 미구현 | 실제 속성 테이블 기반 `@supports`/`CSS.supports()`, `@layer`+`!important`+`revert-layer`, `@container`, `@scope`, `240px 1fr` Grid 픽셀 일치 |
| **CPU 폴백 `test_26` (`C02`)** | `-DARCR_ENABLE_FREETYPE=OFF`에서 `test_suite.c:1368` 실패 | `position: fixed` 내부 `position: absolute` 자손에 고정 컨텍스트 미전파 | `layout/arcr_layout.c`에서 `in_fixed_cb` 전파 및 `scroll_y=600` 히트테스트 일치 (`build-min` **40/40 통과**) |
| **단일 창 프레임 소유권 (`W01`)** | 네이티브 캡션 아래 클라이언트 탭 줄 우측에 `_ □ ×` 중복 렌더링 | 테마와 플랫폼 캡션 소유권(`WaterscapeFrameMetrics`) 미분리 | `cap_h=0` 단일 프레임 계약 확립, 현대 테마 중복 캡션 버튼 **0개**, Win2000 고전 프레임 단일 세트, `PerMonitorV2` DPI, `UIA` `WM_GETOBJECT` 구현 |

---

## 2. 빌드·새니타이저·심볼 순수성·크로스 플랫폼 검증 요약

| 빌드 / 검증 대상 | 설정 및 명령 | 결과 | 비고 |
| :--- | :--- | :--- | :--- |
| **Linux x64 Release (`./build`)** | `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ctest --test-dir build` | **40 / 40 통과** (`1.23s`) | 컴파일러 경고 `0`건, `AVX2` 알파 블렌드 `5,152 MPix/s`, 탭 전환 `0.55 ms` |
| **Linux x64 ASan + UBSan** | `-DCMAKE_BUILD_TYPE=Debug -fsanitize=address,undefined` (`arcr_tests`, `arcr_js`) | **40 / 40 통과** | 메모리 오류·정의되지 않은 동작 `0`건, `arcr_js` LSan 누수 `0 B` |
| **Linux Minimal CPU (`build-min`)** | `-DARCR_ENABLE_FREETYPE=OFF -DARCR_ENABLE_LIBPNG=OFF -DWATERSCAPE_ENABLE_X11=OFF` | **40 / 40 통과** (`1.64s`) | `test_26` 스크롤 고정 헤더 히트테스트 포함 전 항목 통과 |
| **배포 JS 모듈 심볼 순수성** | `! nm -D build/libarcr_js_portable.so \| grep -iE "JS_NewRuntime\|JS_NewContext\|quickjs"` | **통과 (외부 심볼 0개)** | `third_party/quickjs` 기본 배포 그래프에서 완전 제외 (Linux `.so` 및 Win32/x64 `.dll` 동일 검증) |
| **Linux X11 실창 & 인스톨러** | `xvfb-run` + `build_deb.sh` + `build_run_installer.sh --check` + 설치/제거 | **통과** | `.deb` (`1.4 MB`), `.run` (`2.1 MB`, SHA-256 무결성 및 `arcr_js` 래퍼·`uninstall.sh` 검증) |
| **Windows 2000 SP4 x86 (`PE 5.0` · `SSE1`)** | `cmake/toolchains/win2000-x86-sse1.cmake` + `objdump -p` + `arcr_tests.exe` + `win32_ui_verify.exe --classic-frame` | **40 / 40 & UI 20 / 20 통과** | `Subsystem 5.0`, 사후 Win2000 정적 DLL 임포트 `0`개, `dwm=0` 단일 고전 프레임·UIA·`WM_DPICHANGED` (`[30,38,45,60,30]`) 통과. *주: GitHub Actions(Run `43415059917`)에서는 Windows Server WoW64 상의 네이티브 실행 및 `PE 5.0` 정적 임포트 검사를 수행하며, 실제 물리 Windows 2000 SP4 PC/VM 부팅 시험과 명확히 구분하여 기록함.* |
| **Windows x64 (`SSE3` + 런타임 `AVX2`/DWM/UIA)** | `cmake/toolchains/win-x64-sse3.cmake` + `arcr_tests.exe` + `win32_ui_verify.exe` + `WaterscapeSetup.exe --silent` | **40 / 40 & UI 20 / 20 통과** | `dwm=1` 단일 프레임 DWM 캡션(`visible_hwnd=1`, 콘솔 `0`), `WM_GETOBJECT` UIA 9노드 트리, F11/F12 인윈도우 도킹, `520×640` 비겹침, `WM_DPICHANGED` (`[30,38,45,60,30]`), 무인 설치/제거 통과 (Run `43415059917`) |

---

## 3. 표준 적합성 모음 결과 (분모 분리 보고)

### 3.1 독립 `ArcradientJS` (`arcr_js`) Test262 스위트 (`tools/run_test262.py`)
- 실행 명령: `python3 tools/run_test262.py --binary ./build/arcr_js --out-json /tmp/test262_report.json`
- **파일 분모**: **77 / 77 파일 통과 (`100.00%`)**
- **실행 변형(Strict / Non-Strict / Module / Async) 분모**: **132 / 132 실행 통과 (`100.00%`, `0.24s`)**
  - `language`: `26 / 26` (`let`/`const` TDZ, 중첩 화살표 매개변수 구조분해 기본값, private 필드 `#x in obj`, `static {}`, 제너레이터/이터레이터 종료 등)
  - `async`: `10 / 10` (`async`/`await` 마이크로태스크 순서, `for await...of`, `Promise.withResolvers`, `Promise.try`, ES2025 Iterator Helpers)
  - `builtins`: `28 / 28` (ES2025 `Set` 집합 연산, 불변 배열 메서드 `toSorted`/`toReversed`/`with`, `Object.groupBy`, `RegExp.escape`, `BigInt`, `TypedArray`, `Proxy`/`Reflect`)
  - `modules`: `16 / 16` (정적/동적 ESM, 순환 live binding, `import.meta`, Top-Level `await`, 구문/링크 오류 격리)
  - `intl`: `36 / 36` (`Intl.Locale`, `DateTimeFormat`, `NumberFormat`, `Collator`, `PluralRules`, `Segmenter`, `RelativeTimeFormat`, `ListFormat`, `DisplayNames`)
  - `gc`: `6 / 6` (`WeakRef`, `FinalizationRegistry`, 순환 클로저 수집), `realm`: `6 / 6` (`$262.createRealm`), `annexB`: `4 / 4`

### 3.2 Web Platform Tests (WPT) 적합성 샤드 (`tools/run_wpt_shard.py`)
- 실행 명령: `python3 tools/run_wpt_shard.py --binary ./build/waterscape --module-dir ./build --out-json /tmp/wpt_shard_report.json`
- **테스트 파일 분모**: **9 / 9 파일 통과 (`100.00%`)** (`testharness.js` 8종 + 픽셀 비교 `reftest` 1쌍)
- **`testharness.js` 서브테스트 분모**: **18 / 18 서브테스트 통과 (`100.00%`)**
  - `/url/url-constructor.html` (`2/2`), `/encoding/textdecoder-utf8.html` (`1/1`), `/html/syntax/parsing/template-and-foster.html` (`3/3`), `/import-maps/bare-specifier.html` (`1/1`), `/dom/events/shadow-retarget-and-custom-elements.html` (`2/2`), `/css/css-cascade/layer-revert-and-scope.html` (`3/3`), `/css/css-contain/container-and-supports.html` (`3/3`), `/fetch/api/fetch-streams-crypto.html` (`3/3`)
- **시각 `reftest` 분모**: **1 / 1 통과** (`/css/reftest/grid-test.html == /css/reftest/grid-ref.html`, `--save-content-png` 뷰포트 픽셀 100% 일치)

---

## 4. `05_VERIFICATION.md` 규격 결과 행 (`N01`~`R01` 전 31개 항목 100% 통과)

재현 명령:
```bash
python3 tools/run_verification_matrix.py \
  --binary ./build/waterscape --module-dir ./build \
  --benchmarks --live-sites --out-dir /tmp/arcr_verify_2026
```

| SHA | OS/CPU/GPU | 빌드 옵션 | ID | 테스트 커밋 | 기대 | 실제 | 실행 명령 | 증거 경로 | 이슈 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 / Vulkan+XShm | Release (`--conformance`) | `N01-http` | `eb6dcf8` | `302 -> /n01/sub/page.html`, `<base>` CSS+PNG `200`, 한글 제목 | `url=.../n01/sub/page.html title=N01 한글 리디렉션 검증 css=True img=True` | `waterscape --verify-url http://127.0.0.1/n01/start --conformance` | `n01_http.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N01-https-ok` | `eb6dcf8` | 신뢰 루트 CA 체인 검증 `security_state=2` + 한글 제목 | `sec=2 title=N01 한글 리디렉션 검증` | `ARCR_SSL_CERT_FILE=ca.pem waterscape --verify-url https://127.0.0.1/n01/start --conformance` | `n01_https_trusted.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N01-tls-fail` | `eb6dcf8` | 비신뢰 인증서 `security_state=3` 및 `tls-error` 분리 표시 | `sec=3 tls_blocked=True` | `waterscape --verify-url https://127.0.0.1/n01/start --conformance` | `n01_https_untrusted.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N01-cred-strip` | `eb6dcf8` | URL 내 `user:pass@` 자격증명이 추적·오류·텔레메트리에서 제거됨 | `leaked=no` | `waterscape --verify-url http://<redacted>@127.0.0.1/n01/sub/page.html --conformance` | `n01_cred_sanitize.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-gzip` | `eb6dcf8` | `gzip` 응답 해제 후 `title=N02-GZIP-OK` | `title=N02-GZIP-OK` | `waterscape --verify-url .../n02/gzip --conformance` | `n02_gzip.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-brotli` | `eb6dcf8` | `br`(Brotli) 응답 해제 후 `title=N02-BROTLI-OK` | `title=N02-BROTLI-OK` | `waterscape --verify-url .../n02/brotli --conformance` | `n02_brotli.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-chunked` | `eb6dcf8` | `Transfer-Encoding: chunked` 조립 `title=N02-CHUNKED-OK` | `title=N02-CHUNKED-OK` | `waterscape --verify-url .../n02/chunked --conformance` | `n02_chunked.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-euckr` | `eb6dcf8` | HTTP 헤더 `EUC-KR` 디코딩 `title=N02-EUCKR-한글완료` | `title=N02-EUCKR-한글완료` | `waterscape --verify-url .../n02/euckr --conformance` | `n02_euckr.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-meta-euckr` | `eb6dcf8` | `<meta charset="euc-kr">` 스니핑 `title=N02-META-EUCKR-한글완료` | `title=N02-META-EUCKR-한글완료` | `waterscape --verify-url .../n02/meta-euckr --conformance` | `n02_meta-euckr.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-cp1252` | `eb6dcf8` | `windows-1252` 화폐/인용부호 디코딩 | `title=N02-CP1252-€-“OK”` | `waterscape --verify-url .../n02/cp1252 --conformance` | `n02_cp1252.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-404` | `eb6dcf8` | `404` 상태 기록 및 응답 본문 렌더링 | `title=N02-404` (`status=404`) | `waterscape --verify-url .../n02/404 --conformance` | `n02_404.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N02-bad-length` | `eb6dcf8` | `Content-Length` 절단 불일치 시 크래시 없이 안전 처리 | `exit=0 title=연결 오류 — Waterscape` | `waterscape --verify-url .../n02/bad-length --conformance` | `n02_badlen.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N03-nonblocking` | `eb6dcf8` | `700ms` 이미지 대기 중 첫 표시 `<450ms` 및 조기 클릭 즉시 반영 | `fp=34.515ms init=N03-FAST-READY click=N03-CLICKED rs=complete` | `waterscape --verify-url .../n03/index.html --early-click 60,65 --conformance` | `n03_nonblocking.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `N03-cancel` | `eb6dcf8` | 교차 탐색 취소 `<250ms`(`<=50ms` 폴링) 및 이전 응답 혼입 `0` | `second_nav_ms=1.852ms title=N03-PAGE2-CLEAN` | `waterscape --verify-url .../n03/index.html --second-url .../n03/page2.html --conformance` | `n03_cancel.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `H01-html5` | `eb6dcf8` | 자동 닫힘 `<p>`, 문자 참조, 표 foster parenting, `<template>`, `<svg>` | `title=H01:p=true:ent=true:fos=true:tpl=true:svg=true` | `waterscape --verify-url .../h01/index.html --eval-js ... --conformance` | `h01_html5.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S01-order` | `eb6dcf8` | classic/defer/module/nomodule/JSON 실행 순서 및 `readyState` 전이 | `inline1:loading\|sync:false\|inline2:true\|defer1:interactive\|mod:interactive\|defer2:42\|dcl:interactive\|load:complete` | `waterscape --verify-url .../s01/index.html --conformance` | `s01_order.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S02-importmap` | `eb6dcf8` | 첫 외부 ESM 2파일 + `importmap` `title=esm-ok`, 오류 `0` | `title=esm-ok console_len=0` | `waterscape --verify-url .../s02/index.html --conformance` | `s02_importmap.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S02-circular` | `eb6dcf8` | 순환 모듈 그래프 단일 평가 및 live binding 갱신 | `title=circ:6:25 console_len=0` | `waterscape --verify-url .../s02/circular.html --conformance` | `s02_circular.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S02-tla` | `eb6dcf8` | Top-Level `await` 비동기 모듈 그래프 평가 | `title=tla:awaited-2026 console_len=0` | `waterscape --verify-url .../s02/tla.html --conformance` | `s02_tla.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S02-dynamic` | `eb6dcf8` | 동적 `import()` Promise 해석 | `title=dyn:esm-ok console_len=0` | `waterscape --verify-url .../s02/dynamic.html --conformance` | `s02_dynamic.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S02-bad-mime` | `eb6dcf8` | `text/plain` 모듈 거부 및 URL·단계·원인 콘솔 기록 | `title=mime-unchanged console_len=183` | `waterscape --verify-url .../s02/bad_mime.html --conformance` | `s02_bad_mime.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S02-link-err` | `eb6dcf8` | 미존재 export 링크 단계 `SyntaxError` 진단 및 평가 차단 | `title=link-unchanged console_len=306` | `waterscape --verify-url .../s02/link_error.html --conformance` | `s02_link_err.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S03-dom-loop` | `eb6dcf8` | CustomElements, Shadow DOM retargeting, Promise/microtask/MO/timer/rAF | `ce-connected:3,ce-attr:4,retarget:true,promise1,micro1,mo:1,timer,raf` | `waterscape --verify-url .../s03/index.html --conformance` | `s03_dom_loop.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S04-fetch-cors` | `eb6dcf8` | 리디렉션 `response.url`, `ReadableStream`, CORS 허용/차단, `AbortController`, XHR 헤더 | `redir:true:true:redirected-ok\|stream:true\|corsOk:true\|corsDeny:true\|abort:true\|xhr:true` | `waterscape --verify-url .../s04/index.html --conformance` | `s04_fetch_cors.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `S05-storage-wk` | `eb6dcf8` | `structuredClone`, Storage, `IndexedDB`, `BroadcastChannel`, `Worker` | `clone:true\|storage:true\|idb:true\|bc:true\|worker:true` | `waterscape --verify-url .../s05/index.html --conformance` | `s05_storage_wk.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `C01-css2026` | `eb6dcf8` | `@layer`/`revert-layer`, `240px 1fr` Grid, `@container`, `@supports`, `oklch` | `layer:280\|col1:240\|col2x:true\|cq:320\|sup:true` | `waterscape --verify-url .../c01/index.html --conformance` | `c01_css2026.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `C02-fixed-hit` | `eb6dcf8` | `scroll_y=600` 후 고정 헤더 내부 링크 클릭 탐색 일치 | `title=N03-PAGE2-CLEAN url=.../n03/page2.html` | `waterscape --verify-url .../c02/index.html --scroll-y 600 --click 930,22 --conformance` | `c02_fixed_hit.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `A01-hydration` | `eb6dcf8` | SSR → ESM hydration → 클릭·폼 → `fetch` → `pushState` → `history.back()` | `A01-HYDRATED:details:c=1:items=3->A01-HYDRATED:home:c=1:items=3` | `waterscape --verify-url .../a01/index.html --click 60,115 --eval-js ... --conformance` | `a01_step1.json`, `a01_step2.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `A02-media-gpu` | `eb6dcf8` | Canvas 2D 픽셀, `AudioContext`, 정직한 `webgl`/`webgpu` `null` 및 `serviceWorker` 부재 | `title=A02:canvas=true:audio=true:gpuHonest=true` | `waterscape --verify-url .../a02/index.html --conformance` | `a02_media_gpu.json` | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`96`·`192 DPI`) | `W01-frame-dpi` | `eb6dcf8` | 중복 캡션 버튼 `0` (`cap_h=0`), `96->192 DPI` 비례 스케일, 우측 DevTools 도킹 | `cap_h=0 tab96=36 tab192=72 dock_w=720` | `waterscape --verify-url waterscape://addons --dpi 192 --devtools right` | `w01_dpi192_dock_right.png` | 없음 |
| `eb6dcf8` | Windows Server x64 & WoW64 x86 / AVX2 / DWM+UIA | MinGW Release (`PE 5.0` & `x64`) | `W01-win32-native` | `eb6dcf8` | 단일 가시 `HWND`, 콘솔 `0`, `WM_GETOBJECT` UIA 9노드, F11/F12, `520×640` 비겹침, `WM_DPICHANGED` 왕복 | `x86=20/20 x64=20/20 omnibar_px=[30,38,45,60,30]` | `win32_ui_verify.exe --waterscape waterscape.exe --out-json win32_ui_report.json` | `waterscape-windows-ui-verify` (Run `43415059917`) | 없음 |
| `eb6dcf8` | Linux 6.12 x86_64 / Xeon AVX2 | Release (`--conformance`) | `R01-tier4-lazy` | `eb6dcf8` | 무스크립트 내부 문서에서 Tier-4 JS 미적재 (`js_module_loaded=false`) | `exit=0 js_loaded=False` | `waterscape --verify-url waterscape://hardware --conformance` | `r01_lazy_tier4.json` | 없음 |

---

## 5. 성능 및 메모리 벤치마크 (Cold / Warm P50 / P95)

### 5.1 페이지 유형별 Cold / Warm 로딩 및 아레나/RSS 측정 (`iterations = 9`, `--conformance`)

| 워크로드 | 대상 픽스처 | Cold 첫 표시 (`ms`) | Warm P50 첫 표시 (`ms`) | P95 첫 표시 (`ms`) | P50 전체 완료 (`ms`) | P95 전체 완료 (`ms`) | 피크 문서 아레나 (`KiB`) | 프로세스 최대 RSS (`KiB`) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 소형 정적 문서 (`static_n01`) | `/n01/sub/page.html` | `9.263` | **`8.747`** | `9.469` | `9.476` | `10.178` | `160.1` | `46,868` |
| CSS 2026 복합 레이아웃 (`css2026_c01`) | `/c01/index.html` | `34.778` | **`34.669`** | `35.229` | `35.476` | `35.987` | `160.1` | `46,868` |
| ESM SPA Hydration (`spa_a01`) | `/a01/index.html` | `36.571` | **`36.181`** | `37.987` | `37.168` | `38.778` | `160.1` | `46,868` |
| 미디어·Canvas·오디오 (`media_a02`) | `/a02/index.html` | `34.302` | **`36.563`** | `37.058` | `38.118` | `38.599` | `160.1` | `46,868` |

### 5.2 멀티코어 타일 래스터라이저·SIMD·탭 전환 실측치
- **전체 창 프레임 합성 (`1280×800`, 100 프레임, 2 워커 스레드 + `AVX2`)**: **`1,245.1 FPS`** (`0.803 ms/frame`, `1,275.0 MPix/sec`)
- **SIMD 알파 합성 커널 (`test_22`)**: `AVX2 256-bit` **`5,152 MPix/s`** vs `Scalar` **`633 MPix/s`** (**`8.14×` 가속**, 100% 비트 일치)
- **탭 캐시 전환 (`test_33`, `1080×720` 전체 블릿)**: **`0.55 ms`** (`549 µs`, 레이아웃/재래스터화 렉 `0`)

---

## 6. 공개 라이브 사이트 정직한 관측 비교 (`--conformance` vs 기본 모드)

| 사이트 | 모드 | HTTP 상태 | 최종 URL | 문서 `<title>` | DOM 노드 | 요청 수 | 콘솔 JS 오류 | 실제 관측 및 환경 요인 구분 |
| :--- | :--- | :---: | :--- | :--- | ---: | ---: | :---: | :--- |
| **Google** (`google.com`) | `--conformance` | `200` | `https://www.google.com/` | `Google` | `312` | `9` | `0`건 | **순수 표준 엔진만으로 완전 렌더링**: 컬러 로고, 둥근 그림자 검색창, 음성/렌즈 아이콘, `Google 검색` 버튼, 하단 한국어 링크 및 OneGoogle `fetch` RPC 정상 동작 |
| **Google** (`google.com`) | 기본 모드 | `200` | `https://www.google.com/` | `Google` | `312` | `9` | `0`건 | `--conformance`와 동일한 DOM·시각 품질 및 검색 폼 제출 지원 |
| **주전자닷컴** (`zuzunza.com`) | `--conformance` | `200` | `https://www.zuzunza.com/_compat/entry?path=%2F` | `스레드 \| ZUKU` | `281` | `22` | `0`건 | **순수 표준 엔진으로 22개 CSS/JS 청크 로드 완료**: 클라이언트 라우팅 후 상단 검색창·`로그인` 버튼·좌측 네비게이션(`홈`/`Hype`/`Swipe`/`스튜디오` 등) 정상 표시 (메인 피드 본문은 비로그인 API 세션 상태에 따름) |
| **주전자닷컴** (`zuzunza.com`) | 기본 모드 | `200` | `https://www.zuzunza.com/_compat/entry?path=%2F` | `스레드 \| ZUKU` | `281` | `22` | `0`건 | 본문이 비어 있지 않아 하이드레이터 개입 없이 순수 렌더링 결과 그대로 유지됨 |
| **YouTube** (`youtube.com`) | `--conformance` | `200` | `https://www.youtube.com/` | `YouTube` | `445` | `12` | `0`건 | `webcomponents-sd.js`·`scheduler.js`·`spf.js`·`network.js`·`www-main-desktop.css` **12개 리소스 무오류 실행** 및 3열 카드 스켈레톤 그리드 렌더링. 단, 비로그인 루트 `/`는 서버가 빈 추천 초기 데이터를 내려주며 전체 Polymer/CustomElements 번들 완료에는 MSE/대규모 Web Components 추가 고도화가 필요함 |
| **YouTube** (`youtube.com`) | 기본 모드 (`/watch`·검색) | `200` | `https://www.youtube.com/watch?v=...` | `<영상 제목> - YouTube` | `452` | `14` | `0`건 | `ytInitialPlayerResponse` / `ytInitialData`가 포함된 워치·피드 문서에서 기본 실사용 시네마 하이드레이터가 `<video>` 플레이어 및 관련 영상 카드를 구성함 |
| **X** (`x.com`) | `--conformance` | `200` | `https://x.com/` | `X. 무슨 일이 일어나고 있나요? / X` | `301` | `2` | `0`건 | **순수 표준 엔진만으로 공식 한국어 랜딩/로그인 화면 완전 렌더링** (`지금 일어나고 있는 일.`, Google/Apple 로그인 버튼, 아이디 입력창, 대형 X 벡터 로고). 공개 타임라인 열람은 X 서버의 비로그인 게이트 정책에 의해 로그인 벽 뒤로 제한됨 |

---

## 7. 표준 인벤토리 및 산출물 색인
- 기계 판독 표준 인벤토리: [docs/standards_2026_inventory.json](../standards_2026_inventory.json) (전 구현 파일 존재 검증 완료; 미구현 항목인 `HTTP/3·QUIC·WebTransport`, `WebGPU/WGSL/WebGL/Wasm`, `EME/MSE/WebRTC`, `ServiceWorker`는 가짜 스텁 없이 정직하게 `unimplemented`/`partial`로 기록)
- P0 기준선 보고서: [docs/verification/p0_baseline_evidence.md](p0_baseline_evidence.md)
- 검증 실행 도구: [tools/fixture_server.py](../../tools/fixture_server.py), [tools/run_test262.py](../../tools/run_test262.py), [tools/run_wpt_shard.py](../../tools/run_wpt_shard.py), [tools/run_verification_matrix.py](../../tools/run_verification_matrix.py), [tools/win32_ui_verify.c](../../tools/win32_ui_verify.c)
