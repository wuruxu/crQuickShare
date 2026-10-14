// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/crquickshare_window.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/timer.h"
#include "crQuickShare/grit/crquickshare_strings.h"
#include "crQuickShare/quick_share_service.h"
#include "crQuickShare/file_storage.h"
#include "crQuickShare/ble_wakeup.h"
#include "crQuickShare/ble_discovery.h"
#include "base/logging.h"
#include "libcr/titlebar_controls.h"
#include "ui/base/hit_test.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/progress_bar.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/window/client_view.h"
#include "ui/views/window/frame_view.h"
#include "ui/views/window/hit_test_utils.h"

namespace crquickshare {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
class ShareFrame : public views::FrameView {
  METADATA_HEADER(ShareFrame, views::FrameView)
 public:
  explicit ShareFrame(views::Widget* widget) : widget_(widget) {
    title_ = AddChildView(std::make_unique<views::Label>(u"crQuickShare"));
    title_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title_->SetCanProcessEventsWithinSubtree(false);
    auto add = [&](libcr::CaptionButtonType type, int id, base::RepeatingClosure cb) {
      auto* b = AddChildView(std::make_unique<libcr::CaptionButton>(std::move(cb), type));
      b->SetTooltipText(Text(id)); views::SetHitTestComponent(b, HTCLIENT); return b;
    };
    minimize_ = add(libcr::CaptionButtonType::kMinimize, IDS_CQS_MINIMIZE, base::BindRepeating([](views::Widget* w) { w->Minimize(); }, widget));
    maximize_ = add(libcr::CaptionButtonType::kMaximize, IDS_CQS_MAXIMIZE, base::BindRepeating([](views::Widget* w) { w->Maximize(); }, widget));
    restore_ = add(libcr::CaptionButtonType::kRestore, IDS_CQS_RESTORE, base::BindRepeating([](views::Widget* w) { w->Restore(); }, widget));
    close_ = add(libcr::CaptionButtonType::kClose, IDS_CQS_CLOSE, base::BindRepeating([](views::Widget* w) { w->Close(); }, widget));
  }
  gfx::Rect GetBoundsForClientView() const override { return gfx::Rect(1, 36, std::max(0, width() - 2), std::max(0, height() - 37)); }
  gfx::Rect GetWindowBoundsForClientBounds(const gfx::Rect& bounds) const override { return gfx::Rect(bounds.x() - 1, bounds.y() - 36, bounds.width() + 2, bounds.height() + 37); }
  bool HasWindowTitle() const override { return true; }
  bool IsWindowTitleVisible() const override { return true; }
  int NonClientHitTest(const gfx::Point& point) override {
    if (!GetLocalBounds().Contains(point)) return HTNOWHERE;
    for (auto* b : {minimize_.get(), maximize_.get(), restore_.get(), close_.get()}) if (b->GetVisible() && b->bounds().Contains(point)) return HTCLIENT;
    if (!widget_->IsMaximized()) {
      int hit = GetHTComponentForFrame(point, gfx::Insets(6), 6, 6, true);
      if (hit != HTNOWHERE) return hit;
    }
    if (point.y() < 36) return HTCAPTION;
    return HTCLIENT;
  }
  void ResetWindowControls() override { InvalidateLayout(); }
  void UpdateWindowTitle() override {}
  void Layout(PassKey) override {
    widget_->client_view()->SetBoundsRect(GetBoundsForClientView());
    title_->SetBounds(16, 0, std::max(0, width() - 140), 36);
    minimize_->SetBounds(width() - 108, 0, 36, 36);
    maximize_->SetBounds(width() - 72, 0, 36, 36);
    restore_->SetBoundsRect(maximize_->bounds());
    maximize_->SetVisible(!widget_->IsMaximized()); restore_->SetVisible(widget_->IsMaximized());
    close_->SetBounds(width() - 36, 0, 36, 36);
  }
  void OnPaint(gfx::Canvas* canvas) override {
    canvas->FillRect(GetLocalBounds(), GetColorProvider()->GetColor(ui::kColorSysHeader));
  }
 private:
  raw_ptr<views::Widget> widget_;
  raw_ptr<views::Label> title_;
  raw_ptr<libcr::CaptionButton> minimize_, maximize_, restore_, close_;
};
BEGIN_METADATA(ShareFrame)
END_METADATA

class ShareView : public views::View, public ui::SelectFileDialog::Listener {
  METADATA_HEADER(ShareView, views::View)
 public:
  ShareView() {
    auto* command = base::CommandLine::ForCurrentProcess();
    base::FilePath home; base::PathService::Get(base::DIR_HOME, &home);
    destination_ = command->GetSwitchValuePath("receive-dir");
    if (destination_.empty() || !destination_.IsAbsolute()) destination_ = home.Append("Downloads").Append("crQuickShare");
    std::string name = command->GetSwitchValueNative("device-name");
    if (name.empty() || !base::IsStringUTF8(name) || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) name = "crQuickShare";
    service_ = std::make_unique<QuickShareService>(name, destination_);
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(24), 12));
    Label(this, IDS_CQS_SUBTITLE);
    auto* actions = AddChildView(std::make_unique<views::View>());
    actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    receive_ = Button(actions, IDS_CQS_RECEIVE, base::BindRepeating(&ShareView::ToggleReceive, base::Unretained(this)));
    choose_ = Button(actions, IDS_CQS_CHOOSE, base::BindRepeating(&ShareView::Choose, base::Unretained(this), false));
    choose_folder_ = Button(actions, IDS_CQS_CHOOSE_FOLDER, base::BindRepeating(&ShareView::Choose, base::Unretained(this), true));
    Label(this, IDS_CQS_FOLDER_HINT);
    auto* wake_actions = AddChildView(std::make_unique<views::View>());
    auto* wake_layout = wake_actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    wake_ = Button(wake_actions, IDS_CQS_WAKE, base::BindRepeating(&ShareView::ToggleWake, base::Unretained(this)));
    wake_status_ = Label(wake_actions, IDS_CQS_WAKE_STARTING);
    wake_layout->SetFlexForView(wake_status_, 1);
    wake_status_->SetVisible(false);
    ble_status_ = Label(this, IDS_CQS_BLE_SCANNING);
    visibility_ = Label(this, IDS_CQS_HIDDEN);
    selection_ = Label(this, IDS_CQS_NO_FILES);
    Label(this, IDS_CQS_DEVICES);
    auto* scroll = AddChildView(std::make_unique<views::ScrollView>());
    scroll->ClipHeightTo(100, 220);
    peers_ = scroll->SetContents(std::make_unique<views::View>());
    peers_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
    layout->SetFlexForView(scroll, 1);
    status_ = Label(this, IDS_CQS_READY);
    auto* details_scroll = AddChildView(std::make_unique<views::ScrollView>());
    details_scroll->ClipHeightTo(0, 160);
    detail_ = details_scroll->SetContents(std::make_unique<views::Label>());
    detail_->SetMaximumWidth(720);
    detail_->SetMultiLine(true); detail_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    detail_->SetElideBehavior(gfx::ELIDE_TAIL);
    code_ = AddChildView(std::make_unique<views::Label>());
    code_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    progress_ = AddChildView(std::make_unique<views::ProgressBar>());
    progress_->SetValue(0);
    auto* confirmation = AddChildView(std::make_unique<views::View>());
    confirmation->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    accept_ = Button(confirmation, IDS_CQS_ACCEPT, base::BindRepeating(&ShareView::Confirm, base::Unretained(this), true));
    reject_ = Button(confirmation, IDS_CQS_REJECT, base::BindRepeating(&ShareView::Confirm, base::Unretained(this), false));
    cancel_ = Button(confirmation, IDS_CQS_CANCEL, base::BindRepeating([](ShareView* self) { self->service_->Cancel(); }, base::Unretained(this)));
    auto* folder = AddChildView(std::make_unique<views::Label>(Text(IDS_CQS_SAVE_TO) + u" " + destination_.LossyDisplayName()));
    folder->SetHorizontalAlignment(gfx::ALIGN_LEFT); folder->SetElideBehavior(gfx::ELIDE_MIDDLE);
    timer_.Start(FROM_HERE, base::Milliseconds(150), this, &ShareView::Refresh);
    Refresh();
  }
  gfx::Size GetMinimumSize() const override { return gfx::Size(660, 580); }
  ~ShareView() override { timer_.Stop(); if (picker_) picker_->ListenerDestroyed(); }
  void OnThemeChanged() override { views::View::OnThemeChanged(); SetBackground(views::CreateSolidBackground(GetColorProvider()->GetColor(ui::kColorSysSurface))); }
  void FileSelected(const ui::SelectedFileInfo& file, int) override { MultiFilesSelected({file}); }
  void MultiFilesSelected(const std::vector<ui::SelectedFileInfo>& files) override {
    paths_.clear(); for (const auto& f : files) paths_.push_back(f.local_path);
    selection_->SetText(l10n_util::GetStringFUTF16(IDS_CQS_SELECTED, base::NumberToString16(paths_.size())));
    wake_until_ = paths_.empty() ? base::TimeTicks() : base::TimeTicks::Now() + base::Minutes(2);
    service_->RefreshDiscovery();
    picker_.reset(); revision_ = UINT64_MAX; Refresh();
  }
  void FileSelectionCanceled() override { picker_.reset(); }
 private:
  static views::Label* Label(views::View* parent, int id) {
    auto* label = parent->AddChildView(std::make_unique<views::Label>(Text(id)));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true); return label;
  }
  static views::MdTextButton* Button(views::View* parent, int id, base::RepeatingClosure cb) {
    return parent->AddChildView(std::make_unique<views::MdTextButton>(std::move(cb), Text(id)));
  }
  void ToggleWake() {
    wake_until_ = base::TimeTicks::Now() < wake_until_ ? base::TimeTicks() : base::TimeTicks::Now() + base::Minutes(2);
    if (!wake_until_.is_null()) service_->RefreshDiscovery();
    Refresh();
  }
  void ToggleReceive() { service_->SetReceiving(!service_->GetSnapshot().receiving); Refresh(); }
  void Choose(bool folder) {
    if (picker_) return;
    picker_ = ui::SelectFileDialog::Create(this, nullptr);
    picker_->SelectFile(folder ? ui::SelectFileDialog::SELECT_FOLDER : ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE, Text(folder ? IDS_CQS_CHOOSE_FOLDER : IDS_CQS_CHOOSE), {}, nullptr, 0, {}, GetWidget()->GetNativeWindow(), nullptr);
  }
  void Send(Peer peer) { service_->Send(peer, paths_); Refresh(); }
  void Confirm(bool accept) { service_->Confirm(accept); accept_->SetEnabled(false); reject_->SetEnabled(false); }
  void Refresh() {
    auto s = service_->GetSnapshot();
    if (s.busy) wake_until_ = {};
    bool waking = !paths_.empty() && !s.busy && s.available && base::TimeTicks::Now() < wake_until_;
    // Bluetooth callbacks do not change the network snapshot revision.
    // Update wake status before the snapshot-only early return.
    bool visible = GetWidget() && GetWidget()->IsVisible() && !GetWidget()->IsMinimized();
    const char* wake_reason = s.busy ? "connection_active" : paths_.empty() ? "no_selection" :
        !s.available ? "lan_unavailable" : !waking ? "stopped_or_expired" :
        !visible ? "window_hidden" : picker_ ? "file_picker" : "finding_peers";
    if (last_wake_reason_ != wake_reason) {
      last_wake_reason_ = wake_reason;
      LOG(INFO) << "crQuickShare: Discovery assistance; reason=" << wake_reason
                << "; lan_peers=" << s.peers.size();
    }
    wakeup_.Update(waking && visible && !picker_);
    wake_->SetEnabled(!paths_.empty() && !s.busy && s.available);
    wake_->SetText(Text(waking ? IDS_CQS_WAKE_STOP : IDS_CQS_WAKE));
    int wake_id = wakeup_.state() == BleWakeup::State::kAdvertising ? IDS_CQS_WAKE_ACTIVE :
        wakeup_.state() == BleWakeup::State::kUnavailable ? IDS_CQS_WAKE_UNAVAILABLE : IDS_CQS_WAKE_STARTING;
    wake_status_->SetText(Text(wake_id));
    wake_status_->SetVisible(waking && visible && !picker_);
    const bool scanning = visible && !s.busy && !picker_;
    discovery_.Update(scanning);
    ble_status_->SetVisible(scanning);
    ble_status_->SetText(Text(discovery_.state() == BleDiscovery::State::kUnavailable ? IDS_CQS_BLE_UNAVAILABLE : IDS_CQS_BLE_SCANNING));
    auto discovered = discovery_.MergeWithLan(s.peers);
    if (std::any_of(discovered.begin(), discovered.end(), [](const Peer& peer) { return peer.ble_only; }))
      ble_status_->SetText(Text(IDS_CQS_BLE_HINT));
    std::erase_if(discovered, [](const Peer& peer) { return peer.ble_only; });
    if (revision_ == s.revision && listed_peers_ == discovered && s.phase != Phase::kTransferring) return; revision_ = s.revision;
    receive_->SetText(Text(s.receiving ? IDS_CQS_STOP_RECEIVING : IDS_CQS_RECEIVE));
    receive_->SetEnabled(s.available); choose_->SetEnabled(!s.busy); choose_folder_->SetEnabled(!s.busy);
    visibility_->SetText(Text(!s.available ? IDS_CQS_NETWORK_ERROR : s.receiving ? IDS_CQS_VISIBLE : IDS_CQS_HIDDEN));
    if (listed_peers_ != discovered || listed_busy_ != s.busy || listed_paths_ != paths_.size()) {
      listed_peers_ = discovered; listed_busy_ = s.busy; listed_paths_ = paths_.size();
      peers_->RemoveAllChildViews();
      if (discovered.empty()) Label(peers_, IDS_CQS_NO_DEVICES);
      for (const auto& peer : discovered) {
        auto* button = peers_->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&ShareView::Send, base::Unretained(this), peer), (peer.name.empty() ? Text(IDS_CQS_BLE_DEVICE) : base::UTF8ToUTF16(peer.name)) + u"  ·  " + (peer.ble_only ? Text(IDS_CQS_BLE_ONLY) : base::UTF8ToUTF16(peer.address))));
        auto tooltip = Text(peer.ble_only ? IDS_CQS_BLE_HINT : IDS_CQS_SEND);
        if (!peer.bluetooth_address.empty()) tooltip += u"\n" + base::UTF8ToUTF16(peer.bluetooth_address);
        button->SetTooltipText(tooltip); button->SetEnabled(!peer.ble_only && !s.busy && !paths_.empty() && paths_.size() <= kMaxTransferFiles);
      }
      peers_->InvalidateLayout();
    }
    int id = IDS_CQS_READY;
    switch (s.phase) {
      case Phase::kIdle: break;
      case Phase::kConnecting: id = IDS_CQS_CONNECTING; break;
      case Phase::kConfirm: id = s.incoming ? IDS_CQS_INCOMING : IDS_CQS_CONFIRM; break;
      case Phase::kRestoringNetwork: id = IDS_CQS_RESTORING_NETWORK; break;
      case Phase::kWaiting: id = IDS_CQS_WAITING; break;
      case Phase::kTransferring: id = s.incoming ? IDS_CQS_RECEIVING : IDS_CQS_SENDING; break;
      case Phase::kComplete: id = IDS_CQS_COMPLETE; break;
      case Phase::kSent: id = IDS_CQS_SENT; break;
      case Phase::kRejected: id = IDS_CQS_REJECTED; break;
      case Phase::kCancelled: id = IDS_CQS_CANCELLED; break;
      case Phase::kTimedOut: id = IDS_CQS_TIMED_OUT; break;
      case Phase::kError: id = s.failure == Failure::kHotspotUnavailable ? IDS_CQS_HOTSPOT_REQUIRED : IDS_CQS_ERROR; break;
    }
    status_->SetText(Text(id));
    std::u16string details = base::UTF8ToUTF16(s.peer_name);
    if (s.current_file_index) details += u"\n" + base::UTF8ToUTF16(s.current_file);
    else for (size_t i = 0; i < s.files.size(); ++i) details += u"\n" + base::UTF8ToUTF16(s.files[i]);
    if (!s.files.empty()) details += u"\n" + ui::FormatBytes(base::ByteSize(s.transferred)) + u" / " + ui::FormatBytes(base::ByteSize(s.total));
    if (s.current_file_index) {
      details += u"\n" + Text(IDS_CQS_PROGRESS_FILE) + u": " +
          base::NumberToString16(s.current_file_index) + u" / " + base::NumberToString16(s.files.size()) +
          u" · " + ui::FormatBytes(base::ByteSize(s.current_file_transferred)) + u" / " +
          ui::FormatBytes(base::ByteSize(s.current_file_total));
      details += u"\n" + Text(IDS_CQS_PROGRESS_FINISHED) + u": " +
          base::NumberToString16(s.completed_files) + u" / " + base::NumberToString16(s.files.size());
      if (s.busy && s.phase == Phase::kTransferring) {
        details += u"\n" + Text(IDS_CQS_PROGRESS_SPEED) + u": " +
            ui::FormatSpeed(base::ByteSize(s.bytes_per_second));
        details += u" · " + Text(IDS_CQS_PROGRESS_REMAINING) + u": " +
            (s.remaining_seconds < 0 ? std::u16string(u"—") :
             base::NumberToString16(s.remaining_seconds / 60) + u":" +
             (s.remaining_seconds % 60 < 10 ? u"0" : u"") + base::NumberToString16(s.remaining_seconds % 60));
      }
    }
    if (!s.received_directory.empty()) details += u"\n" + s.received_directory.LossyDisplayName();
    if (s.network_cleanup_failed) details += u"\n" + Text(IDS_CQS_NETWORK_CLEANUP_FAILED);
    detail_->SetText(details);
    code_->SetText(s.code.empty() || !s.busy ? std::u16string() : l10n_util::GetStringFUTF16(IDS_CQS_CODE, base::UTF8ToUTF16(s.code)));
    progress_->SetValue(s.phase == Phase::kComplete ? 1.0 : s.total ? double(s.transferred) / s.total : s.files.empty() ? 0.0 : double(s.completed_files) / s.files.size());
    accept_->SetVisible(s.phase == Phase::kConfirm); reject_->SetVisible(s.phase == Phase::kConfirm);
    if (s.phase != last_phase_) { accept_->SetEnabled(true); reject_->SetEnabled(true); last_phase_ = s.phase; }
    cancel_->SetVisible(s.busy && s.phase != Phase::kRestoringNetwork);
    InvalidateLayout();
  }
  BleWakeup wakeup_;
  BleDiscovery discovery_;
  base::TimeTicks wake_until_;
  std::string last_wake_reason_;
  std::unique_ptr<QuickShareService> service_;
  base::FilePath destination_;
  std::vector<base::FilePath> paths_;
  scoped_refptr<ui::SelectFileDialog> picker_;
  base::RepeatingTimer timer_;
  uint64_t revision_ = UINT64_MAX;
  std::vector<Peer> listed_peers_;
  bool listed_busy_ = true;
  size_t listed_paths_ = SIZE_MAX;
  Phase last_phase_ = Phase::kIdle;
  raw_ptr<views::View> peers_;
  raw_ptr<views::Label> visibility_, selection_, status_, detail_, code_, wake_status_, ble_status_;
  raw_ptr<views::ProgressBar> progress_;
  raw_ptr<views::MdTextButton> receive_, choose_, choose_folder_, accept_, reject_, cancel_, wake_;
};
BEGIN_METADATA(ShareView)
END_METADATA
class ShareDelegate : public views::WidgetDelegate {
 public:
  explicit ShareDelegate(base::OnceClosure quit) : quit_(std::move(quit)) {
    SetHasWindowSizeControls(true); SetTitle(u"crQuickShare");
    SetContentsView(std::make_unique<ShareView>());
  }
  std::unique_ptr<views::FrameView> CreateFrameView(views::Widget* widget) override { return std::make_unique<ShareFrame>(widget); }
  void WindowClosing() override { if (quit_) std::move(quit_).Run(); }
 private:
  base::OnceClosure quit_;
};
std::unique_ptr<views::Widget>& Window() { static base::NoDestructor<std::unique_ptr<views::Widget>> window; return *window; }
std::unique_ptr<ShareDelegate>& Delegate() { static base::NoDestructor<std::unique_ptr<ShareDelegate>> delegate; return *delegate; }
}
void ShowCrQuickShareWindow(base::OnceClosure quit) {
  Delegate() = std::make_unique<ShareDelegate>(std::move(quit));
  Window() = std::make_unique<views::Widget>();
  views::Widget::InitParams params(views::Widget::InitParams::CLIENT_OWNS_WIDGET, views::Widget::InitParams::TYPE_WINDOW);
  params.delegate = Delegate().get(); params.bounds = gfx::Rect(860, 720);
  params.name = "crquickshare_window"; params.remove_standard_frame = true;
  params.wm_class_name = "libcr.crquickshare"; params.wm_class_class = "libcr.crquickshare"; params.wayland_app_id = "libcr.crquickshare";
  Window()->Init(std::move(params)); Window()->CenterWindow(gfx::Size(860, 720)); Window()->Show();
}
void DestroyCrQuickShareWindow() { Window().reset(); Delegate().reset(); }
}
