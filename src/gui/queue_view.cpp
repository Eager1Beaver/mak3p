#include "gui/queue_view.hpp"
#include "gui/constants.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <algorithm>

namespace audio_converter::gui {
namespace {
std::string shortened(std::string value, int width) {
    if (fl_width(value.c_str()) <= width) return value;
    while (!value.empty() && fl_width((value + "…").c_str()) > width) {
        do { value.pop_back(); } while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0) == 0x80);
        // Remove the leading byte of a partially removed multibyte character.
        if (!value.empty() && static_cast<unsigned char>(value.back()) >= 0xc0) value.pop_back();
    }
    return value + "…";
}
}
QueueView::QueueView(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    scrollbar_ = new Fl_Scrollbar(x + w - metrics::scrollbar_width, y, metrics::scrollbar_width, h);
    scrollbar_->type(FL_VERTICAL); scrollbar_->step(metrics::row_height / 2);
    scrollbar_->callback([](Fl_Widget*, void* data) { static_cast<QueueView*>(data)->redraw(); }, this);
    end();
}
void QueueView::update_scrollbar() {
    const int total = static_cast<int>(rows_.size()) * metrics::row_height;
    scrollbar_->value(std::clamp(scrollbar_->value(), 0, std::max(0, total - h())), h(), 0, std::max(h(), total));
    if (total > h()) scrollbar_->show(); else scrollbar_->hide();
}
void QueueView::rows(std::vector<RowText> rows, bool editable) {
    bool changed = editable != editable_ || rows.size() != rows_.size();
    if (!changed) for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& a = rows[i]; const auto& b = rows_[i];
        if (a.id != b.id || a.name != b.name || a.properties != b.properties || a.status != b.status ||
            a.remaining != b.remaining || a.progress != b.progress || a.warning != b.warning || a.failed != b.failed ||
            a.progress_visible != b.progress_visible) { changed = true; break; }
    }
    rows_ = std::move(rows); editable_ = editable; update_scrollbar(); if (changed) redraw();
}
void QueueView::resize(int x, int y, int w, int h) {
    Fl_Group::resize(x, y, w, h);
    scrollbar_->resize(x + w - metrics::scrollbar_width, y, metrics::scrollbar_width, h);
    update_scrollbar();
}
void QueueView::draw() {
    fl_push_clip(x(), y(), w(), h()); fl_color(colors::queue); fl_rectf(x(), y(), w(), h());
    const int usable = w() - (scrollbar_->visible() ? metrics::scrollbar_width : 0);
    const int status_width = static_cast<int>(usable * metrics::status_width_ratio);
    const int left_width = usable - status_width;
    for (std::size_t i = static_cast<std::size_t>(scrollbar_->value() / metrics::row_height); i < rows_.size(); ++i) {
        const int top = y() + static_cast<int>(i) * metrics::row_height - scrollbar_->value();
        if (top >= y() + h()) break;
        const auto& row = rows_[i];
        fl_color(i % 2 ? colors::row_even : colors::row_odd);
        fl_rectf(x(), top, usable, metrics::row_height);
        fl_font(metrics::font_face, metrics::font_size); fl_color(colors::text);
        fl_draw(shortened(row.name, left_width - 18).c_str(), x() + 9, top + metrics::row_name_baseline);
        fl_font(metrics::font_face, metrics::small_font_size); fl_color(colors::muted);
        fl_draw(shortened(row.properties, left_width - 18).c_str(), x() + 9, top + metrics::row_properties_baseline);
        const int status_x = x() + left_width;
        if (row.warning || row.failed) {
            fl_color(row.failed ? colors::error : colors::warning);
            fl_circle(status_x + 6, top + 19, 6); fl_draw("!", status_x + 6.5, top + 24.5);
        }
        fl_color(row.failed ? colors::error : fl_rgb_color(40, 57, 74));
        fl_draw(shortened(row.status, status_width - 47).c_str(), status_x + 17, top + 23);
        fl_color(editable_ ? fl_rgb_color(55, 65, 78) : fl_rgb_color(175, 180, 187));
        const int close_x = x() + usable - 18;
        fl_line(close_x - 4, top + 15, close_x + 4, top + 23); fl_line(close_x + 4, top + 15, close_x - 4, top + 23);
        if (row.progress_visible) {
            const int bar_width = std::max(30, left_width / 3);
            fl_color(colors::progress_track); fl_rectf(x() + 9, top + metrics::row_progress_offset, bar_width, 5);
            fl_color(colors::accent);
            if (row.progress) fl_rectf(x() + 9, top + metrics::row_progress_offset, static_cast<int>(bar_width * *row.progress), 5);
            else fl_rectf(x() + 9, top + metrics::row_progress_offset, bar_width / 3, 5);
            fl_color(colors::muted);
            fl_draw(shortened(row.remaining, usable - bar_width - 30).c_str(), x() + bar_width + 18, top + metrics::row_remaining_baseline);
        }
    }
    if (rows_.empty()) { fl_font(metrics::font_face, metrics::font_size); fl_color(fl_rgb_color(100, 110, 125));
        fl_draw(text("gui_empty").c_str(), x() + 12, y() + 12, usable - 24, h() - 24, FL_ALIGN_CENTER | FL_ALIGN_WRAP); }
    draw_children(); fl_color(colors::border); fl_rect(x(), y(), w(), h()); fl_pop_clip();
}
int QueueView::handle(int event) {
    const int usable = w() - (scrollbar_->visible() ? metrics::scrollbar_width : 0);
    const int index = (Fl::event_y() - y() + scrollbar_->value()) / metrics::row_height;
    if (event == FL_DND_ENTER || event == FL_DND_DRAG || event == FL_DND_RELEASE) {
        if (!editable_) return 0;
        Fl::belowmouse(this);
        return 1;
    }
    if (event == FL_DND_LEAVE) return 1;
    if (event == FL_PASTE) { if (editable_ && drop) drop(std::string_view(Fl::event_text(), Fl::event_length())); return 1; }
    if (event == FL_MOUSEWHEEL) { scrollbar_->value(std::clamp(scrollbar_->value() + Fl::event_dy() * 28, 0,
        std::max(0, static_cast<int>(rows_.size()) * metrics::row_height - h()))); redraw(); return 1; }
    if (index >= 0 && static_cast<std::size_t>(index) < rows_.size() && Fl::event_x() < x() + usable) {
        if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && Fl::event_x() >= x() + usable - 32) {
            if (editable_ && remove) remove(rows_[index].id);
            return 1;
        }
        if (event == FL_MOVE || event == FL_ENTER) {
            const auto value = Fl::event_x() >= x() + usable - 32 ? text("gui_remove") : rows_[index].tooltip;
            if (hover_ != value) { Fl_Tooltip::exit(this); hover_ = value;
                Fl::belowmouse(this);
                Fl_Tooltip::enter_area(this, 0, index * metrics::row_height - scrollbar_->value(), usable, metrics::row_height, hover_.c_str()); }
            return 1;
        }
    }
    if (event == FL_LEAVE) { Fl_Tooltip::exit(this); hover_.clear(); }
    return Fl_Group::handle(event);
}

} // namespace audio_converter::gui
