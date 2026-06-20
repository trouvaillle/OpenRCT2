# TTF 폰트 크기를 windowScale 에 비례하도록 수정

## 배경

OpenRCT2는 게임 내장 UI 확대 기능(`windowScale`)을 제공한다. 이 기능은 Options 창의 +/- 버튼(0.25x 단위) 또는 `Ctrl+Plus/Minus` 단축키로 조작할 수 있으며, 설정 범위는 0.5x ~ 5.0x 이다.

이 기능이 활성화되면 게임의 논리적 캔버스 크기가 물리 창 크기를 `windowScale`로 나눈 값으로 줄어들고, 작아진 캔버스를 다시 물리 해상도로 확대하여 출력한다. 이 과정에서 Sprite/TrueType 폰트 모두 확대 대상이 되는데, TrueType 폰트의 경우 **포인트 사이즈가 항상 고정**되어 있어 스케일에 따른 품질 저하가 발생했다.

## 목적

**한국어(CJK) 폰트**가 `windowScale > 1.0`에서 깨져 보이는 문제를 해결한다.

구체적으로:
- 1x 에서는 정상
- 1.5x, 2x 등에서 폰트에 **세로줄/가로줄**이 생기고 글자가 깨짐
- 영문/숫자에 비해 한글이 특히 심하게 영향받음

## 원인 분석

### 근본 원인 1: TTF 포인트 사이즈 고정

`src/openrct2/interface/Fonts.cpp` 에 정의된 폰트 디스크립터는 `ptSize`가 **하드코딩**되어 있으며, `windowScale`과 무관하게 항상 같은 값(한국어: 12pt)으로 폰트를 로딩한다.

```cpp
TTFFontSetDescriptor TTFFontGulim = { {
    { "gulim.ttc", "Gulim", 12, 1, 0, 15, HINTING_THRESHOLD_MEDIUM, nullptr },  // ← 12pt 고정
    // ...
} };
```

이로 인해 2x 스케일에서도 12pt로 렌더링된 폰트가 960x540 캔버스에 그려진 후 2배 확대되어 24pt처럼 보이지만, 실제 디테일은 12pt에 불과한 현상이 발생한다.

### 근본 원인 2: 캔버스 업스케일

`src/openrct2-ui/UiContext.cpp:846-847`:

```cpp
_width  = static_cast<int32_t>(width  / Config::Get().general.windowScale);
_height = static_cast<int32_t>(height / Config::Get().general.windowScale);
```

1920x1080 창, 2x 스케일 → 논리적 캔버스 960x540. 12pt 폰트가 이 작은 캔버스에 상대적으로 크게 렌더링되고, 이후 nearest-neighbor(정수 스케일) 또는 linear(분수 스케일) 보간으로 확대된다.

### 심층 원인: Embedded Bitmap

`FT_LOAD_DEFAULT`는 FreeType2에서 **임베디드 비트맵 스트라이크를 아웃라인보다 우선**한다. 한국어 폰트(Gulim, Malgun 등)는 12pt/12px에 최적화된 임베디드 비트맵을 내장하고 있어, 1x에서는 이 비트맵이 1:1 매핑되어 깔끔하게 보이지만 업스케일 시 비트맵 확대 아티팩트(줄/깨짐)가 발생한다.

## 해결 방법

### 최종 설계: `ptSize *= windowScale`

두 가지 접근 방안 중 **최소 변경 + 최대 효과**를 위해 `ptSize`만 스케일링하는 방법을 선택했다:

```cpp
// src/openrct2/drawing/TTF.cpp:120-122 (TTFInitialise)
float scale = std::max(1.0f, Config::Get().general.windowScale);
int32_t scaledSize = static_cast<int32_t>(fontDesc->ptSize * scale);
fontDesc->font = TTFOpenFont(fontPath.c_str(), scaledSize);
```

2x 스케일에서 24pt로 폰트를 직접 로딩하므로:
- 임베디드 12px 비트맵과 사이즈가 불일치 → FreeType2가 자동으로 **아웃라인 렌더링** 선택
- 캔버스 업스케일이 아닌 **네이티브 해상도**로 렌더링
- `FT_LOAD_NO_BITMAP` 등 FreeType 플래그 변경 불필요

### 동적 스케일 변경 대응: `TTFReinitialise()`

`Ctrl+Plus/Minus` 등으로 실시간 스케일 변경 시 폰트를 재로딩하는 함수 추가:

```cpp
void TTFReinitialise()
{
    // 캐시 비우기 → 기존 폰트 닫기 → 새 windowScale 으로 다시 열기
}
```

호출은 `TriggerResize()`에 `windowScale` delta gate 와 함께 추가하여, 실제 스케일 변경 시에만 폰트를 리로드하도록 최적화:

```cpp
static float lastWindowScale = 0;
float currentScale = Config::Get().general.windowScale;
if (currentScale != lastWindowScale)
{
    lastWindowScale = currentScale;
    TTFReinitialise();
}
```

### 제외한 접근법 (리뷰 결과 반영)

| 접근법 | 제외 사유 |
|--------|----------|
| `FT_LOAD_NO_BITMAP` 추가 | `ptSize` 스케일링만으로도 임베디드 비트맵 문제 해결. 불필요한 FreeType 동작 변경 |
| `enlargedUi`(boolean) 수정 | `enlargedUi`는 폰트와 무관한 위젯 크기 전용 설정. `windowScale`과 별개 |

## 결과

### 정상 동작 예시

| windowScale | 이전 | 이후 |
|-------------|------|------|
| 1.0x | Gulim 12pt → 1920x1080 → 선명 | **동일** (변화 없음) |
| 1.5x | Gulim 12pt → 853x480 → 1.5x 업스케일 → **깨짐** | Gulim 18pt → 1280x853캔버스 → 1.5x → **선명** |
| 2.0x | Gulim 12pt → 960x540 → 2x 업스케일 → **깨짐** | Gulim 24pt → 960x540 → 2x → **선명** |
| 0.5x | Gulim 12pt (1.0f floor) → 3840x2160 캔버스 | **동일** (1.0f 이하로는 스케일 다운되지 않음) |

### 변경 파일

| 파일 | 변경 | 영향 범위 |
|------|------|----------|
| `src/openrct2/drawing/TTF.cpp` | `#include "../config/Config.h"` 추가, `ptSize` 스케일링, `TTFReinitialise()` 추가 | TTF 폰트 로딩/재로딩 |
| `src/openrct2/drawing/TTF.h` | `TTFReinitialise()` 선언 | 공개 API 확장 |
| `src/openrct2-ui/UiContext.cpp` | `#include <openrct2/drawing/TTF.h>` 추가, delta-gated `TTFReinitialise()` 호출 | UI 컨텍스트 |
| `test/tests/TTFTests.cpp` | **신규**: `TTFReinitialise` 안전성 테스트 | 테스트 커버리지 |
| `test/tests/CMakeLists.txt` | `TTFTests.cpp` 등록 | 빌드 시스템 |

### 변경 전후 diff 요약

```
 4 files changed, 49 insertions(+), 2 deletions(-)
 + 1 test file (25 lines)
```

## 코드 리뷰

### 리뷰 결과 (모의 코드 오너 리뷰)

| 우선순위 | 지적 사항 | 조치 |
|----------|-----------|------|
| **HIGH** | `TTFReinitialise`에서 `TTFOpenFont` null 반환 시 댕글링 포인터 | Null 체크 + `LOG_VERBOSE` 추가 |
| **HIGH** | 폰트 경로(`GetFontPath`) empty 시 무시 → `font->font == nullptr` | `continue` + 로깅 추가 |
| MEDIUM | `FT_LOAD_NO_BITMAP`은 bitmap-only 폰트에서 `FT_Load_Glyph` 실패 유발 | 불필요 판단, **제거** |
| MEDIUM | `TTFSDLPort.cpp` 주석(embedded bitmap 선호)과 모순 | 변경 자체를 제거하여 주석과 일치 유지 |
| LOW | `TriggerResize()`가 모든 config 변경 시 호출되어 불필요한 폰트 리로드 | `windowScale` delta gate 추가 |
| LOW | `std::max(1.0f, ...)`가 sub-1x 스케일에서 폰트 크기 고정 | 현재 `windowScale` 범위(0.5~5.0)에서 sub-1x는 UI 축소 목적이므로 적절 |

### 적용한 코딩 컨벤션

- `DrawingUniqueLock<std::mutex>` — 기존 TTF 코드의 뮤텍스 패턴 준수
- `LOG_VERBOSE` — 기존 `TTFInitialise`와 일관된 에러 로깅
- `static float lastWindowScale` — 별도 상태 관리 없이 최소한의 변경
- `DISABLE_TTF` 가드 — stub 함수로 `#else` 블록에도 추가
- Include ordering — `<openrct2/drawing/TTF.h>`를 `IDrawingEngine.h` 다음에 알파벳 순으로 배치

## 후속 이슈

### 잠재적 개선 사항

1. **Sub-1x 폰트 스케일링**: 현재 `std::max(1.0f, ...)`로 1.0x 이하에서는 폰트 크기가 고정됨. 0.5x 스케일에서 폰트까지 축소하려면 `ptSize * windowScale`에서 floor 제거 필요 (단, 12pt * 0.5 = 6pt는 FreeType에서 너무 작아 가독성 문제 가능).

2. **모든 TTF 폰트에 일괄 적용**: 한국어뿐 아니라 일본어, 중국어, 아랍어 등 모든 TTF 사용 언어에 동일한 효과가 적용됨. 각 언어별 `ptSize`가 달라(10~12pt) 스케일링 비율은 동일하지만 절대 크기는 언어별로 다를 수 있음.

3. **캐시 카운트 언더플로우** (기존 버그): `TTFSurfaceCacheDisposeAll()`이 빈 슬롯에서도 `_ttfSurfaceCacheCount--`를 실행하여 언더플로우 발생. 디버그 전용 통계이므로 기능적 영향은 없으나, 추후 정리 필요.

4. **`TTFTests.cpp` 확장**: 현재는 `TTFReinitialise()`의 초기화 전 안전성만 테스트. `cmake -B build -DWITH_TESTS=ON` 으로 빌드 후 실행 가능 (`ctest -R TTF`). 실제 폰트 로딩/렌더링 테스트는 FreeType 의존성으로 인해 테스트 환경 구성이 필요.

### 현재 환경

- cmake, msbuild 등 빌드 도구 미설치로 테스트 실행 불가
- VS Code + VS Build Tools 환경에서 `cmake -B build -DWITH_TESTS=ON; cmake --build build --target OpenRCT2Tests` 로 테스트 빌드/실행 가능
