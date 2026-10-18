#include "GraphOverlays.h"

#include "project/ColorAssetLoader.h"
#include "project/ScreenGraphProject.h"

// The look: a dark ground, light ink, the lit tile in the accent the
// navigator uses, the current screen outlined.
static const uint32_t GROUND = 0x202020;
static const uint32_t TILE = 0x303030;
static const uint32_t LIT = 0x4060ff;
static const uint32_t INK = 0xffffff;
static const uint32_t JOIN = 0x808080;
static const int ICON = 56;

static uint16_t rgb565(uint32_t c) {
  return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

bool OverlayIcon::make(const String& iconPath, uint16_t ink, uint16_t bg) {
  release();
  path = iconPath;
  fg = ink;
  if (path.isEmpty()) return false;
  canvas = new ClippedCanvas16(ICON, ICON);
  if (canvas == nullptr || canvas->getBuffer() == nullptr) {
    release();
    return false;
  }
  canvas->fillScreen(bg);
  ColorAssetLoader::drawGrayscaleMaskToCanvas(canvas, path, 0, 0, fg, bg);
  dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  dsc.header.always_zero = 0;
  dsc.header.reserved = 0;
  dsc.header.w = ICON;
  dsc.header.h = ICON;
  dsc.data_size = ICON * ICON * sizeof(lv_color_t);
  dsc.data = (const uint8_t*)canvas->getBuffer();
  return true;
}

void OverlayIcon::repaint(uint16_t bg) {
  if (canvas == nullptr) return;
  canvas->fillScreen(bg);
  ColorAssetLoader::drawGrayscaleMaskToCanvas(canvas, path, 0, 0, fg, bg);
}

void OverlayIcon::release() {
  delete canvas;
  canvas = nullptr;
  dsc = {};
}

// A tile or a hint: the icon, the name under it.
static lv_obj_t* makeCard(lv_obj_t* parent, int w, int h, uint32_t ground, OverlayIcon* icon, const String& name) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, h);
  lv_obj_set_style_bg_color(box, lv_color_hex(ground), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 10, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  if (icon != nullptr && icon->canvas != nullptr) {
    lv_obj_t* img = lv_img_create(box);
    lv_img_set_src(img, &icon->dsc);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);
  }
  lv_obj_t* label = lv_label_create(box);
  lv_obj_set_width(label, w - 8);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(INK), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_label_set_text(label, name.c_str());
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
  return box;
}

// ------------------------------------------------------------------ hints

void GraphHints::show(const ProjectConfig& project, int screenIndex) {
  hide();
  hints_.reserve(4);
  for (screengraph::Edge side : screengraphproject::tabSidesAt(project, screenIndex)) {
    const int target = screengraphproject::neighbourIndex(project, screenIndex, side);
    if (target < 0) continue;
    const Screen& screen = project.screens[target];
    // Made where it stays: the image points at the icon's descriptor.
    hints_.emplace_back();
    Hint& hint = hints_.back();
    hint.side = screengraph::edgeName(side);
    hint.icon.make(screen.pageIconPath, rgb565(INK), rgb565(GROUND));
    hint.box = makeCard(lv_layer_top(), 100, 88, GROUND, &hint.icon, screen.name);
    switch (side) {
      case screengraph::Edge::Up: lv_obj_align(hint.box, LV_ALIGN_TOP_MID, 0, 14); break;
      case screengraph::Edge::Down: lv_obj_align(hint.box, LV_ALIGN_BOTTOM_MID, 0, -14); break;
      case screengraph::Edge::Left: lv_obj_align(hint.box, LV_ALIGN_LEFT_MID, 14, 0); break;
      case screengraph::Edge::Right: lv_obj_align(hint.box, LV_ALIGN_RIGHT_MID, -14, 0); break;
    }
  }
  opacity_ = -1;
  setTravel(0);
}

void GraphHints::setTravel(int px) {
  // Full at the start of a swipe, gone once the screen has come a third of
  // the way.
  const int fade = 120;
  const int opa = px >= fade ? 0 : (int)LV_OPA_COVER * (fade - px) / fade;
  if (opa == opacity_) return;
  opacity_ = opa;
  for (Hint& h : hints_) lv_obj_set_style_opa(h.box, (lv_opa_t)opa, 0);
}

void GraphHints::hide() {
  for (Hint& h : hints_) {
    if (h.box) lv_obj_del(h.box);
    h.icon.release();
  }
  hints_.clear();
}

String GraphHints::sides() const {
  String out;
  for (const char* e : {"up", "down", "left", "right"}) {
    for (const Hint& h : hints_) {
      if (h.side == e) out += (out.isEmpty() ? "" : " ") + h.side;
    }
  }
  return out;
}

// -------------------------------------------------------------------- map

bool GraphMapView::open(const ProjectConfig& project, int screenIndex, int size) {
  close();
  size_ = size;
  // The comb from the main row and what lies above and below its pages.
  std::vector<graphmap::Column> columns;
  for (const std::string& id : screengraphproject::mainRowIds(project)) {
    const int i = screengraphproject::screenIndexById(project, String(id.c_str()));
    if (i < 0) continue;
    columns.push_back({id, screengraphproject::neighbourId(project, i, screengraph::Edge::Up),
                       screengraphproject::neighbourId(project, i, screengraph::Edge::Down)});
  }
  tiles_ = graphmap::layout(columns);
  current_ = graphmap::indexOf(tiles_, project.screens[screenIndex].id.c_str());
  if (current_ < 0 || tiles_.size() < 2) {
    tiles_.clear();
    current_ = -1;
    return false;
  }
  for (const graphmap::Tile& t : tiles_) screenOf_.push_back(screengraphproject::screenIndexById(project, String(t.id.c_str())));

  backdrop_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(backdrop_);
  lv_obj_set_size(backdrop_, size, size);
  lv_obj_set_style_bg_color(backdrop_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(backdrop_, LV_OPA_90, 0);
  lv_obj_clear_flag(backdrop_, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // The joins first, under the tiles: the row's neighbours, and each page's
  // screen above and below.
  for (size_t i = 0; i < tiles_.size(); i++) {
    for (size_t j = i + 1; j < tiles_.size(); j++) {
      const graphmap::Tile &a = tiles_[i], &b = tiles_[j];
      const bool rowNeighbours = a.level == 0 && b.level == 0 && std::abs(a.col - b.col) == 1;
      const bool docked = a.col == b.col && (a.level == 0 || b.level == 0);
      if (!rowNeighbours && !docked) continue;
      joins_.push_back({(int)i, (int)j});
      lv_obj_t* line = lv_obj_create(backdrop_);
      lv_obj_remove_style_all(line);
      lv_obj_set_style_bg_color(line, lv_color_hex(JOIN), 0);
      lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
      joinObjs_.push_back(line);
    }
  }
  icons_.resize(tiles_.size());
  for (size_t i = 0; i < tiles_.size(); i++) {
    const Screen& screen = project.screens[screenOf_[i]];
    icons_[i].make(screen.pageIconPath, rgb565(INK), rgb565(TILE));
    lv_obj_t* tile = makeCard(backdrop_, graphmap::TILE_W, graphmap::TILE_H, TILE, &icons_[i], screen.name);
    if ((int)i == current_) {
      lv_obj_set_style_border_color(tile, lv_color_hex(INK), 0);
      lv_obj_set_style_border_width(tile, 2, 0);
    }
    tileObjs_.push_back(tile);
  }
  lit_ = current_;
  paintTile(lit_, true);
  pan_ = {0, 0};
  lastMs_ = 0;
  place();
  return true;
}

void GraphMapView::place() {
  if (!backdrop_) return;
  for (size_t i = 0; i < tiles_.size(); i++) {
    const graphmap::Point c = graphmap::centreOf(tiles_[i], tiles_[current_], size_, pan_);
    lv_obj_set_pos(tileObjs_[i], c.x - graphmap::TILE_W / 2, c.y - graphmap::TILE_H / 2);
  }
  for (size_t k = 0; k < joins_.size(); k++) {
    const graphmap::Point a = graphmap::centreOf(tiles_[joins_[k].first], tiles_[current_], size_, pan_);
    const graphmap::Point b = graphmap::centreOf(tiles_[joins_[k].second], tiles_[current_], size_, pan_);
    if (a.y == b.y) {
      lv_obj_set_pos(joinObjs_[k], std::min(a.x, b.x), a.y - 1);
      lv_obj_set_size(joinObjs_[k], std::abs(a.x - b.x), 2);
    } else {
      lv_obj_set_pos(joinObjs_[k], a.x - 1, std::min(a.y, b.y));
      lv_obj_set_size(joinObjs_[k], 2, std::abs(a.y - b.y));
    }
  }
}

void GraphMapView::pointer(int x, int y, unsigned long nowMs) {
  if (!backdrop_) return;
  const long ms = lastMs_ == 0 ? 0 : (long)(nowMs - lastMs_);
  lastMs_ = nowMs;
  const graphmap::Point next = graphmap::panned(tiles_, current_, size_, pan_, {x, y}, ms);
  const int lit = graphmap::tileAt(tiles_, current_, size_, next, {x, y});
  if (next.x == pan_.x && next.y == pan_.y && lit == lit_) return;
  pan_ = next;
  if (lit != lit_) {
    if (lit_ >= 0) paintTile(lit_, false);
    if (lit >= 0) paintTile(lit, true);
    lit_ = lit;
  }
  place();
}

void GraphMapView::paintTile(int i, bool lit) {
  if (i < 0 || i >= (int)tileObjs_.size()) return;
  const uint32_t ground = lit ? LIT : TILE;
  lv_obj_set_style_bg_color(tileObjs_[i], lv_color_hex(ground), 0);
  if (icons_[i].canvas != nullptr) {
    icons_[i].repaint(rgb565(ground));
    // The image shows the canvas it points at; LVGL redraws it when told.
    lv_obj_invalidate(tileObjs_[i]);
  }
}

int GraphMapView::close() {
  const int chosen = (backdrop_ && lit_ >= 0 && lit_ != current_) ? screenOf_[lit_] : -1;
  if (backdrop_) lv_obj_del(backdrop_);
  backdrop_ = nullptr;
  for (OverlayIcon& icon : icons_) icon.release();
  icons_.clear();
  tiles_.clear();
  screenOf_.clear();
  tileObjs_.clear();
  joins_.clear();
  joinObjs_.clear();
  current_ = -1;
  lit_ = -1;
  return chosen;
}

String GraphMapView::debugJson() const {
  String out = "{\"open\":" + String(backdrop_ ? "true" : "false");
  out += ",\"lit\":\"" + String(lit_ >= 0 && lit_ < (int)tiles_.size() ? tiles_[lit_].id.c_str() : "") + "\"";
  out += ",\"tiles\":[";
  for (size_t i = 0; i < tiles_.size(); i++) {
    const graphmap::Point c = graphmap::centreOf(tiles_[i], tiles_[current_], size_, pan_);
    if (i) out += ",";
    out += "{\"id\":\"" + String(tiles_[i].id.c_str()) + "\",\"x\":" + String(c.x) + ",\"y\":" + String(c.y) + "}";
  }
  out += "]}";
  return out;
}
