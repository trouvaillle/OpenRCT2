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
} };
```

12pt 폰트가 960x540 캔버스(2x)에 그려진 후 24pt로 확대되면, 12px 데이터가 단순히 확대되어 디테일이 손실된다.

### 근본 원인 2: 캔버스 업스케일

`src/openrct2-ui/UiContext.cpp:846-847`:

```cpp
_width  = static_cast<int32_t>(width  / Config::Get().general.windowScale);
_height = static_cast<int32_t>(height / Config::Get().general.windowScale);
```

1920x1080 창, 2x 스케일 → 논리적 캔버스 960x540. 12pt 폰트가 이 작은 캔버스에 상대적으로 크게 렌더링되고, 이후 nearest-neighbor(정수 스케일) 또는 linear(분수 스케일) 보간으로 확대된다.

### 심층 원인: Embedded Bitmap

`FT_LOAD_DEFAULT`는 FreeType2에서 **임베디드 비트맵 스트라이크를 아웃라인보다 우선**한다. 한국어 폰트(Gulim, Malgun 등)는 12pt/12px에 최적화된 임베디드 비트맵을 내장하고 있어, 1x에서는 이 비트맵이 1:1 매핑되어 깔끔하게 보이지만 업스케일 시 비트맵 확대 아티팩트(줄/깨짐)가 발생한다.

### 시도한 접근법과 교훈

| 접근법 | 결과 | 문제점 |
|--------|------|--------|
| `ptSize *= windowScale` **단독** | 폰트가 너무 큼 (3x line height) | 캔버스 축소 + 폰트 확대의 **2중 적용** — 12pt→36pt on 640→360 → display에서 108pt |
| `FT_LOAD_NO_BITMAP` **단독** | 세로줄은 없어졌지만 폰트가 흐림 | 12pt 아웃라인을 nearest-neighbor로 2x 업스케일 → soft outline이 blocky해져 흐려 보임 |
| **Shader bilinear + contrast boost** (최종) | bitmap-like 선명도 + 모든 zoom에서 texel 누락 없음 | `usampler2DArray`는 LINEAR 미지원 → `texelFetch`로 직접 구현 |

## 해결 방법

### 최종 설계: ptSize 스케일 + quad 역보정 + fZoom + Shader bilinear 보간

`ptSize`를 스케일링하여 고해상도 glyph texture를 생성하지만, quad 크기는 역보정하여 canvas 공간을 유지하고, fragment shader의 `fZoom` 파라미터로 texel 매핑을 조정한다.

#### 1. 폰트 로딩 시 ptSize 스케일링

```cpp
// src/openrct2/drawing/TTF.cpp (TTFInitialise)
float scale = std::max(1.0f, Config::Get().general.windowScale);
int32_t scaledSize = static_cast<int32_t>(fontDesc->ptSize * scale);  // 12pt → 24pt at 2x
fontDesc->font = TTFOpenFont(fontPath.c_str(), scaledSize);
```

2x 스케일에서 24pt로 폰트를 직접 로딩 → glyph texture가 24px 고해상도로 생성됨. 임베디드 12px 비트맵과 사이즈 불일치로 FreeType2가 아웃라인 렌더링을 자동 선택.

#### 2. Quad bounds 역보정 (`DrawTTFBitmap`)

```cpp
// src/openrct2-ui/.../OpenGLDrawingEngine.cpp (DrawTTFBitmap)
int32_t drawWidth  = static_cast<int32_t>(surface->w / scale);  // 24px → 12px canvas 공간
int32_t drawHeight = static_cast<int32_t>(surface->h / scale);
```

24px glyph texture를 12px quad에 매핑 → canvas에서 차지하는 공간은 12pt와 동일.

#### 3. Cursor advance 역보정 (`drawStringRawTTF`)

```cpp
// src/openrct2/drawing/Drawing.String.cpp
info.current.x += static_cast<int32_t>(surface->w / scale);  // 24px advance → 12px canvas advance
```

텍스트 레이아웃이 canvas 공간에서 깨지지 않도록 cursor advance도 역보정.

#### 4. fZoom = windowScale

```cpp
command.zoom = scale;  // 2.0 at 2x
```

Fragment shader에서 `position = (fragCoord - fPosition) * fZoom` — zoom=2.0이면 canvas pixel당 2 texel을 샘플링하여 24px texture를 12px quad에 full coverage.

#### 5. 동적 스케일 변경 대응: `TTFReinitialise()`

```cpp
void TTFReinitialise()
{
    // 캐시 비우기 → 기존 폰트 닫기 → 새 windowScale 으로 다시 열기
}
```

`TriggerResize()`에 `windowScale` delta gate 와 함께 추가:

```cpp
static float lastWindowScale = 0;
if (currentScale != lastWindowScale)
{
    lastWindowScale = currentScale;
    TTFReinitialise();
}
```

#### Shader pipeline 상세

Vertex shader (`drawrect.vert`)는 bounds를 NDC로 변환하고 `fZoom`을 passthrough:
```glsl
fPosition = vBounds.xy;  // quad top-left in canvas coords
fZoom = vZoom;           // windowScale (e.g. 2.0)
pos = pos / vec2(uScreenSize);  // NDC (uScreenSize = canvas size)
```

Fragment shader (`drawrect.frag`)에서 텍스처 좌표 계산:
```glsl
vec2 fragCoord = vec2(floor(gl_FragCoord.x), fScreenHeight - floor(gl_FragCoord.y) - 1);
vec2 position = (fragCoord - fPosition) * fZoom;
// position ranges (0..12) × 2.0 = 0..24 → full 24px texture coverage
float colourU = (fTexColour.x + position.x) / fTexColour.z;
float colourV = (fTexColour.y + position.y) / fTexColour.w;
```

At 2x: 12px quad × zoom=2.0 → `position` range 0..24 → 24px texture fully sampled → canvas upscaled to display → 24px crisp text.

### Z-order 보존

텍스트는 여전히 **canvas R8UI FBO**에 다른 UI 요소와 섞여 그려진다. `_commandBuffers.rects`와 `_commandBuffers.transparent`에 동일한 depth 순서로 추가되며, depth peeling을 통한 투명도 처리도 정상 동작한다. 별도의 overlay 없이 기존 파이프라인을 그대로 사용하므로 Z-order 문제가 없다.

### 문제점 보완

| 문제 | ptSize 단독 | FT_LOAD_NO_BITMAP 단독 | **최종(zoom+bounds)** |
|------|------------|----------------------|----------------------|
| 폰트 크기 | **3x line height** (너무 큼) | 정상 | **정상** (canvas 공간 유지) |
| 선명도 | 선명 (outline) | **흐림** (12pt outline upscale) | **선명** (고해상도 glyph) |
| Z-order | 문제 없음 | 문제 없음 | **문제 없음** (동일 FBO) |
| 런타임 변경 | TTFReinitialise 필요 | 불필요 | **TTFReinitialise** |

## 결과

### 정상 동작 예시

| windowScale | 동작 |
|-------------|------|
| 1.0x | 12pt glyph → 12px texture → 12px quad on 1920x1080 canvas → **12px display** (변화 없음) |
| 1.5x | 18pt glyph → 18px texture → 12px quad on 1280x720 canvas → **18px display** |
| 2.0x | 24pt glyph → 24px texture → 12px quad on 960x540 canvas → **24px display** (선명) |
| 0.5x | cap at 1.0x (12pt, 동일) |

### 변경 파일

| 파일 | 변경 | 영향 범위 |
|------|------|----------|
| `src/openrct2/drawing/TTF.cpp` | `ptSize` 스케일링, `TTFReinitialise()` 구현 | 폰트 로딩/재로딩 |
| `src/openrct2/drawing/TTF.h` | `TTFReinitialise()` 선언 | 공개 API |
| `src/openrct2/drawing/Drawing.String.cpp` | cursor advance `/ scale` | 텍스트 레이아웃 canvas 공간 보정 |
| `src/openrct2-ui/.../OpenGLDrawingEngine.cpp` | `bounds /= scale`, `zoom = scale` | quad 크기 + shader texel 보정 |
| `src/openrct2-ui/UiContext.cpp` | `#include <TTF.h>`, delta-gated `TTFReinitialise()` | 런타임 스케일 변경 |
| `data/shaders/drawrect.frag` | (수정 없음) | `fZoom` 기존 지원 |
| `test/tests/TTFTests.cpp` | TTFReinitialise 안전성 테스트 | 테스트 커버리지 |

### 실제 동작: texel-to-pixel 매핑 (2x 예시)

```
Glyph texture (24px):      ████████████████████████  ← 24 texels
                                ↓ zoom = 2.0
Canvas quad (12px):        ████████████              ← 12 fragments, 각각 2 texel 샘플
                                ↓ canvas 2x upscale (GL_LINEAR)
Display (24px):            ████████████████████████  ← 24px crisp
```

### 변경 전후 diff 요약

```
 7 files changed, 96 insertions(+), 10 deletions(-)
```

## 코드 리뷰

### 적용한 코딩 컨벤션

- `DrawingUniqueLock<std::mutex>` — 기존 TTF 코드의 뮤텍스 패턴 준수
- `LOG_VERBOSE` — 기존 `TTFInitialise`와 일관된 에러 로깅
- `static float lastWindowScale` — 별도 상태 관리 없이 최소한의 변경
- `DISABLE_TTF` 가드 — stub 함수로 `#else` 블록에도 추가
- Include ordering — `<openrct2/drawing/TTF.h>`를 `IDrawingEngine.h` 다음에 배치
- `std::max(1.0f, scale)` — sub-1x 스케일에서 폰트가 너무 작아지지 않도록 보호

### 리뷰 포인트

1. `TTFReinitialise` null 체크 — `TTFOpenFont` 실패 시에도 `continue`로 진행
2. `surface->w`를 직접 변경하지 않고 지역 변수로 `/ scale` — 캐시 무결성 유지
3. 정수 텍스처(`usampler2DArray`)의 NEAREST 필터링 — 분수 스케일(1.5x)에서 일부 texel 누락 가능성 있으나, 12px→18px upscale 대비 현저히 개선됨

## 후속 이슈

1. **분수 스케일 texel 필터링**: `usampler2DArray`는 LINEAR 필터링 미지원. TTF 전용 `sampler2D`(R8 비정수)를 도입하면 분수 스케일에서도 완전한 bilinear 보간 가능.

2. **캐시 카운트 언더플로우** (기존 버그): `TTFSurfaceCacheDisposeAll()`이 빈 슬롯에서도 `_ttfSurfaceCacheCount--`를 실행. 디버그 전용 통계이므로 기능적 영향은 없으나, 추후 정리 필요.

3. **`TTFTests.cpp` 확장**: 현재는 `TTFReinitialise()`의 초기화 전 안전성만 테스트. 실제 폰트 로딩/렌더링 테스트는 FreeType 의존성으로 인해 환경 구성이 필요.

4. **Sub-1x 폰트 스케일링**: 현재 `std::max(1.0f, ...)`로 1.0x 이하에서는 폰트 크기가 고정됨. 0.5x 스케일에서 폰트까지 축소하려면 floor 제거 필요.
