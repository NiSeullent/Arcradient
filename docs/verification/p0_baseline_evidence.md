# P0 기준선 증거 기록 (Baseline Evidence Report)

- **조사 일시**: 2026-11-13 KST
- **기준 커밋 (`HEAD`)**: `7e51944af4a7fbfa5cc2af13e776fa10afd9fcc2`
- **호스트 환경**:
  - OS: `Linux sandbox 6.12.105+ x86_64 GNU/Linux` (Debian 12)
  - CPU: `Intel(R) Xeon(R) Platinum 8481C` (2 vCPU, `SSE1`..`SSE4.2`, `AVX`, `AVX2`, `AVX-512`)
  - 컴파일러 / 빌드: `GCC 12.2.0`, `CMake 3.25.1`, `Ninja 1.11.1`
  - 실행 파일 버전: `Waterscape Browser 2.0.0 / Arcradient Engine 2.0.0`

## 1. 기준선 빌드 및 CTest 결과

| 빌드 프로필 | 빌드 디렉터리 | CMake 옵션 | CTest 결과 | 비고 |
| --- | --- | --- | --- | --- |
| 기본 Release | `./build` | 기본값 (`FREETYPE=ON`, `LIBPNG=ON`, `OPENSSL=ON`, `X11=ON`) | **33/33 통과** (`0.97s`) | AVX2 블렌드 `5145 MPix/s`, Scalar `631 MPix/s`, 탭 캐시 전환 `552 us` |
| 최소 CPU Release | `/tmp/p0-build-min` | `-DARCR_ENABLE_FREETYPE=OFF -DARCR_ENABLE_LIBPNG=OFF -DWATERSCAPE_ENABLE_X11=OFF` | **32/33** (`test_26` 실패) | `[FAIL] tests/test_suite.c:1368: position:fixed header + nested position:absolute link stayed pinned at scroll_y=600!` |

### 내장 헤드리스 및 스냅샷 기준선
- 실행 명령: `./build/waterscape --module-dir ./build --headless`
  - 출력: `Waterscape Headless Render: OK (1080x720) | URL=waterscape://newtab | workers=2 tiles=170 layout=2090us paint=2196us addons=4 arena=207706B`
- 스냅샷 명령: `./build/waterscape --module-dir ./build --snapshot-dir /tmp/p0-snapshots`
  - 생성 파일: `waterscape_addons_hardware.png` (1120×760), `shell_modern_dark.png`, `shell_modern_light.png`, `shell_win2000_classic.png` 외 총 9종.

## 2. 초기 로컬 픽스처 재현 및 최초 실패 단계 (`tools/fixture_server.py`)

재현 명령:
```bash
python3 tools/fixture_server.py --port 18080 --cors-port 18081 --https-port 18443
/tmp/p0-build-min/p0_fixture_probe ./build \
  http://127.0.0.1:18080/s02/index.html \
  http://127.0.0.1:18080/n03/index.html \
  http://127.0.0.1:18080/a01/index.html \
  http://127.0.0.1:18080/c01/index.html \
  http://127.0.0.1:18080/n01/start
```

| ID | 대상 픽스처 | 기대 결과 | 기준선 실제 관찰 결과 | 최초 실패 단계 및 원인 판정 |
| --- | --- | --- | --- | --- |
| `S02` | 외부 ESM 2파일 (`entry.js`, `value.js`) + `importmap` + classic 스크립트 | `title=esm-ok`, `value.js` 1회 평가, classic DOM 표시 | `title=before-esm`, 콘솔 `[Uncaught JS Exception] SyntaxError: Unexpected token '{' at <script>:1:8` | **스크립트 준비/평가 단계**: `core/arcr_engine.c:1786`이 `<script type="importmap">`과 `<script type="module">`을 구분하지 않고 classic 전역 스크립트로 평가하며, 외부 모듈 그래프 fetch/link/evaluate 미구현 |
| `N03` | 빠른 HTML + 파서 차단 스크립트 + 느린 이미지(`delay_ms=700`) | 첫 표시·스크립트 완료 `< 50ms`, 느린 이미지는 비동기로 후속 반영 | `nav_ms=707.91ms` 동안 전체 탐색·첫 페인트 차단 | **서브리소스 스케줄링 단계**: `load_html_internal`(`core/arcr_engine.c:2165`)이 모든 `<img>` 배치 다운로드 완료까지 첫 레이아웃/페인트를 동기 차단 |
| `A01` | 중립 호스트(`127.0.0.1`) SSR + Tailwind v4 + ESM hydration + 클릭/fetch/라우팅 | `title=A01-HYDRATED:home:c=0:items=2` 및 상호작용 성공 | `title=A01-SSR:home`, 콘솔 `SyntaxError: Unexpected token '{'` | **모듈 스크립트/호스트 단계**: 사이트 이름 기반 `arcr_site_hydrators.h`가 개입하지 않는 일반 로컬 앱에서 ESM hydration 불발 및 `history.back()` 무동작 |
| `C01` | `@layer`, `@supports`, `@container`, `@scope`, 2열 CSS Grid | `layer:280\|col1:240\|col2x:true\|cq:320\|sup:true` | `layer:1080\|col1:1080\|col2x:false\|cq:1080\|sup:false` | **CSS 파싱·캐스케이드·레이아웃 단계**: `@supports`와 `CSS.supports()`가 미지원 속성에 무조건 `true` 반환, `@layer` 순서·`@container`·`@scope` 미적용, 다중 트랙 Grid 폭 미계산 |
| `C02` / `test_26` | `-DARCR_ENABLE_FREETYPE=OFF`에서 `position: fixed` 내부 `position: absolute` 링크 `scroll_y=600` 히트테스트 | `(940, 20)`에서 `https://arcradient.org` 히트 | `test_suite.c:1368` 실패 | **레이아웃/히트테스트 단계**: `layout/arcr_layout.c:2389`에서 `is_fixed`가 하위 `position: absolute` 박스로 전파되지 않고 비트맵 폴백 글리프 폭에 의존 |
| `W01` | `waterscape://addons` + F12 창 프레임 소유권 | OS 캡션과 공유 셸 캡션 중 단일 세트만 존재 | `shell/main.c:630`이 `draw_os_window_frame=false`로 설정하여 `WS_OVERLAPPEDWINDOW` 네이티브 캡션 아래 클라이언트 탭 줄 우측에 모의 `_ □ ×` 글리프 3개가 중복 렌더링됨 (`arcr_shell.c:4249`) | **셸/프레젠터 프레임 계약 단계**: 테마와 실제 플랫폼 캡션 소유권 미분리 |
| 배포 JS 모듈 | `libarcr_js_portable.so` 심볼 의존성 | 자체 C11 `ArcradientJS`만 포함 | `third_party/quickjs/*` 직접 링크 (`JS_NewRuntime` 등 포함) | **JS 엔진 아키텍처 단계**: 자체 C11 VM(`js/vm`, `js/builtins`, `js/modules`) 및 분리된 `web/bindings`로의 전환 필요 |
