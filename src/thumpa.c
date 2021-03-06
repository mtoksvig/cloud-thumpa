#include "pebble.h"

#define SCREEN_TOTAL_WIDTH 144
#define SCREEN_TOTAL_HEIGHT 168
#define SCREEN_MARGIN 10
#define SCREEN_LEFT 0
#define SCREEN_TOP SCREEN_MARGIN
#define SCREEN_RIGHT SCREEN_TOTAL_WIDTH
#define SCREEN_BOTTOM (SCREEN_TOTAL_HEIGHT-SCREEN_MARGIN)
#define SCREEN_CENTER_X ((SCREEN_LEFT+SCREEN_RIGHT)/2)
#define SCREEN_CENTER_Y ((SCREEN_TOP+SCREEN_BOTTOM)/2)

#define PLASMA true
#define PLASMA_SCALE 20
#define PLASMA_SPEED 13
#define PLASMA_STRIDE 2
#define PLASMA_MASK ((1<<PLASMA_STRIDE)-1)

#define MODEL true
#define MODEL_SPEED 13

#define THUMPA true

bool vibrate = true;
bool backlight = false;

#define LOG PBL_LOG_APP

#include "cobra_model.h"
#include "vec_mat.h"

int model_scale = MODEL_SCALE;

static Window *s_main_window;
static Layer *s_image_layer;
static GBitmap *s_image;

// 33 patterns, each 8w x 4h x 1 bit

// to render grayscale value g at (x, y), you write e.g.:
//   framebuffer |= dither[g][y%4] & (0x1 << (x%8))

// notice that you can render e.g. 4 bits at once:
//   framebuffer |= dither[g][y%4] & (0xF << (x%8))

uint8_t dither[33][4] = { { 0 } };

// extract bit at position position in byte byte
uint8_t bit(uint8_t byte, uint8_t position) {
  return (byte>>position) & 1;
}

// initalize the dither table
void dither_init() {
  for (uint8_t level = 1; level < ARRAY_LENGTH(dither); ++level) {
    // copy previous level
    for (uint8_t y = 0; y < ARRAY_LENGTH(dither[level]); ++y)
      dither[level][y] = dither[level-1][y];
    // we want to distribute the increments as much as possible
    // so put every other bit into x and every other into y
    // and put the lsbs in the msbs and vice versa
    uint8_t i = level-1;
    uint8_t x =
      bit(i, 0) << 2 |
      bit(i, 2) << 1 |
      bit(i, 4) << 0;
    uint8_t y =
      bit(i, 1) << 1 |
      bit(i, 3) << 0;
    // we want a pair of bits to spell out an X, not a Z
    // so xor y into x
    // yeah, head hurts, but it does work, i think
    x ^= y;
    // set the bit
    dither[level][y] |= 1<<x;
  }
}

int8_t sin_table[256] = { 0 };

void sin_table_init() {
  for (unsigned i = 0; i<ARRAY_LENGTH(sin_table); ++i)
    sin_table[i] = sin_lookup(i<<8)>>9;
}

int8_t fast_sin(uint16_t i) {
  return sin_table[i>>8];
}

// take values in the +/-1<<9 range
// remap them to the 0..32 range somehow
int32_t remap(int32_t x) {
  x = (x>>7)+(1<<2); // 0..7
  int32_t result[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
  if (MODEL)
    return result[x]*2+18; // pale or you can't see model
  return result[x]*4+2; // darker
}

void plasma_effect(GBitmap *bitmap) {
  // int16_t w = bitmap->bounds.size.w;
  int16_t h = bitmap->bounds.size.h;
  uint32_t stride = bitmap->row_size_bytes;
  uint8_t *bytes = bitmap->addr;

  static uint16_t fx0, fx1, fy0, fy1;
  uint16_t y0 = fy0, y1 = fy1;
  for (int y = 0; y < h; ++y) {
    int32_t plasma0 = fast_sin(y0) + fast_sin(y1);

    uint16_t x0 = fx0, x1 = fx1;
    for (uint16_t i = 0; i < stride; ++i) {
      uint8_t buf = 0;
      for (uint16_t j = 0; j < 8; j += PLASMA_STRIDE) {
        // compute plasma
        int32_t plasma = plasma0 + fast_sin(x0) + fast_sin(x1);
        // remap into bands
        plasma = remap(plasma);
        // or PLASMA_MASK bits of dither into buf
        buf |= dither[plasma][y%4] & (PLASMA_MASK << j);

        x0 += PLASMA_SCALE*PLASMA_STRIDE*47;
        x1 += PLASMA_SCALE*PLASMA_STRIDE*26;
      }
      bytes[y*stride+i] = buf;
    }
    y0 += PLASMA_SCALE*9;
    y1 += PLASMA_SCALE*33;
  }

  fx0 += PLASMA_SPEED*77;
  fy0 += PLASMA_SPEED*93;
  fx1 += PLASMA_SPEED*-12;
  fy1 += PLASMA_SPEED*17;
}

void draw_plasma(GContext *ctx, GRect bounds) {
  plasma_effect(s_image);
  graphics_draw_bitmap_in_rect(ctx, s_image, bounds);
}

void draw_model(GContext *ctx) {
  // the rotation around z
  static int32_t anglez = 0;
  static int32_t anglex = 0;
  
  graphics_context_set_stroke_color(ctx, GColorBlack);

  // rotate unit matrix around z
  int32_t m0[3][3]; mrotz(m0, munit, anglez); mshift(m0, m0, 8);
  int32_t m[3][3]; mrotx(m, m0, anglex); mshift(m, m, 8);

  // now draw each edge
  for (unsigned i = 0; i < ARRAY_LENGTH(edges); ++i) {
    GPoint a, b;

    screen_transform(&a, m, verts[edges[i][0]], model_scale);
    screen_transform(&b, m, verts[edges[i][1]], model_scale);

    graphics_draw_line(ctx, a, b);
  }

  anglex += MODEL_SPEED*37;
  anglez += MODEL_SPEED*50;
}

// This is a layer update callback where compositing will take place
static void layer_update_callback(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_frame(layer);

  draw_plasma(ctx, bounds);
  draw_model(ctx);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

void handle_timer(void *p) {
  (void)p;
  layer_mark_dirty(s_image_layer);
  app_timer_register(3 /* ms, bad things happen with 0 or 1 */, handle_timer, NULL);
}  

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(s_main_window);
  GRect bounds = layer_get_frame(window_layer);

  s_image_layer = layer_create(bounds);
  layer_set_update_proc(s_image_layer, layer_update_callback);
  layer_add_child(window_layer, s_image_layer);

  s_image = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PUG);
  app_timer_register(3 /* ms, bad things happen with 0 or 1 */, handle_timer, NULL);
}

static void main_window_unload(Window *window) {
  gbitmap_destroy(s_image);
  layer_destroy(s_image_layer);
}

static void init() {
  dither_init();
  sin_table_init();
  
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
