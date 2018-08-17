/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "passport/passport_panel_edit_scans.h"

#include "passport/passport_panel_controller.h"
#include "passport/passport_panel_details_row.h"
#include "info/profile/info_profile_button.h"
#include "info/profile/info_profile_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/text_options.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "boxes/abstract_box.h"
#include "storage/storage_media_prepare.h"
#include "styles/style_boxes.h"
#include "styles/style_passport.h"

namespace Passport {
namespace {

constexpr auto kMaxDimensions = 2048;
constexpr auto kMaxSize = 10 * 1024 * 1024;
constexpr auto kJpegQuality = 89;

static_assert(kMaxSize <= UseBigFilesFrom);

base::variant<ReadScanError, QByteArray> ProcessImage(QByteArray &&bytes) {
	auto image = App::readImage(base::take(bytes));
	if (image.isNull()) {
		return ReadScanError::CantReadImage;
	} else if (!Storage::ValidateThumbDimensions(image.width(), image.height())) {
		return ReadScanError::BadImageSize;
	}
	if (std::max(image.width(), image.height()) > kMaxDimensions) {
		image = std::move(image).scaled(
			kMaxDimensions,
			kMaxDimensions,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	auto result = QByteArray();
	{
		QBuffer buffer(&result);
		if (!image.save(&buffer, QByteArray("JPG"), kJpegQuality)) {
			return ReadScanError::Unknown;
		}
		base::take(image);
	}
	if (result.isEmpty()) {
		return ReadScanError::Unknown;
	} else if (result.size() > kMaxSize) {
		return ReadScanError::FileTooLarge;
	}
	return result;
}

} // namespace

class ScanButton : public Ui::AbstractButton {
public:
	ScanButton(
		QWidget *parent,
		const style::PassportScanRow &st,
		const QString &name,
		const QString &status,
		bool deleted,
		bool error);

	void setImage(const QImage &image);
	void setStatus(const QString &status);
	void setDeleted(bool deleted);
	void setError(bool error);

	rpl::producer<> deleteClicks() const {
		return _delete->entity()->clicks();
	}
	rpl::producer<> restoreClicks() const {
		return _restore->entity()->clicks();
	}

protected:
	int resizeGetHeight(int newWidth) override;

	void paintEvent(QPaintEvent *e) override;

private:
	int countAvailableWidth() const;

	const style::PassportScanRow &_st;
	Text _name;
	Text _status;
	int _nameHeight = 0;
	int _statusHeight = 0;
	bool _error = false;
	QImage _image;
	object_ptr<Ui::FadeWrapScaled<Ui::IconButton>> _delete;
	object_ptr<Ui::FadeWrapScaled<Ui::RoundButton>> _restore;

};

struct EditScans::SpecialScan {
	SpecialScan(ScanInfo &&file);

	ScanInfo file;
	QPointer<Ui::SlideWrap<Ui::FlatLabel>> header;
	QPointer<Ui::VerticalLayout> wrap;
	base::unique_qptr<Ui::SlideWrap<ScanButton>> row;
	QPointer<Info::Profile::Button> upload;
	bool errorShown = false;
	Animation errorAnimation;
	rpl::variable<bool> rowCreated;
};

void UpdateFileRow(
		not_null<ScanButton*> button,
		const ScanInfo &info) {
	button->setStatus(info.status);
	button->setImage(info.thumb);
	button->setDeleted(info.deleted);
	button->setError(!info.error.isEmpty());
}

base::unique_qptr<Ui::SlideWrap<ScanButton>> CreateScan(
		not_null<Ui::VerticalLayout*> parent,
		const ScanInfo &info,
		const QString &name) {
	auto result = base::unique_qptr<Ui::SlideWrap<ScanButton>>(
		parent->add(object_ptr<Ui::SlideWrap<ScanButton>>(
			parent,
			object_ptr<ScanButton>(
				parent,
				st::passportScanRow,
				name,
				info.status,
				info.deleted,
				!info.error.isEmpty()))));
	result->entity()->setImage(info.thumb);
	return result;
}

EditScans::List::List(
	not_null<PanelController*> controller,
	ScanListData &&data)
: controller(controller)
, files(std::move(data.files))
, initialCount(int(files.size()))
, errorMissing(data.errorMissing) {
}

EditScans::List::List(
	not_null<PanelController*> controller,
	base::optional<ScanListData> &&data)
: controller(controller)
, files(data ? std::move(data->files) : std::vector<ScanInfo>())
, initialCount(data ? base::make_optional(int(files.size())) : base::none)
, errorMissing(data ? std::move(data->errorMissing) : QString()) {
}

bool EditScans::List::uploadedSomeMore() const {
	if (!initialCount) {
		return false;
	}
	const auto from = begin(files) + *initialCount;
	const auto till = end(files);
	return std::find_if(from, till, [](const ScanInfo &file) {
		return !file.deleted;
	}) != till;
}

bool EditScans::List::uploadMoreRequired() const {
	if (!upload) {
		return false;
	}
	const auto exists = ranges::find_if(
		files,
		[](const ScanInfo &file) { return !file.deleted; }) != end(files);
	if (!exists) {
		return true;
	}
	const auto errorExists = ranges::find_if(
		files,
		[](const ScanInfo &file) { return !file.error.isEmpty(); }
	) != end(files);
	return (errorExists || uploadMoreError) && !uploadedSomeMore();
}

Ui::SlideWrap<ScanButton> *EditScans::List::nonDeletedErrorRow() const {
	const auto nonDeletedErrorIt = ranges::find_if(
		files,
		[](const ScanInfo &file) {
			return !file.error.isEmpty() && !file.deleted;
		});
	if (nonDeletedErrorIt == end(files)) {
		return nullptr;
	}
	const auto index = (nonDeletedErrorIt - begin(files));
	return rows[index].get();
}

rpl::producer<QString> EditScans::List::uploadButtonText() const {
	return Lang::Viewer(files.empty()
		? lng_passport_upload_scans
		: lng_passport_upload_more) | Info::Profile::ToUpperValue();
}

void EditScans::List::hideError() {
	toggleError(false);
}

void EditScans::List::toggleError(bool shown) {
	if (errorShown != shown) {
		errorShown = shown;
		errorAnimation.start(
			[=] { errorAnimationCallback(); },
			errorShown ? 0. : 1.,
			errorShown ? 1. : 0.,
			st::passportDetailsField.duration);
	}
}

void EditScans::List::errorAnimationCallback() {
	const auto error = errorAnimation.current(errorShown ? 1. : 0.);
	if (error == 0.) {
		upload->setColorOverride(base::none);
	} else {
		upload->setColorOverride(anim::color(
			st::passportUploadButton.textFg,
			st::boxTextFgError,
			error));
	}
}

void EditScans::List::updateScan(ScanInfo &&info, int width) {
	const auto i = ranges::find(files, info.key, [](const ScanInfo &file) {
		return file.key;
	});
	if (i != files.end()) {
		*i = std::move(info);
		const auto scan = rows[i - files.begin()]->entity();
		UpdateFileRow(scan, *i);
		if (!i->deleted) {
			hideError();
		}
	} else {
		files.push_back(std::move(info));
		pushScan(files.back());
		wrap->resizeToWidth(width);
		rows.back()->show(anim::type::normal);
		if (divider) {
			divider->hide(anim::type::normal);
		}
		header->show(anim::type::normal);
		uploadTexts.fire(uploadButtonText());
	}
}

void EditScans::List::pushScan(const ScanInfo &info) {
	const auto index = rows.size();
	const auto type = info.type;
	rows.push_back(CreateScan(
		wrap,
		info,
		lng_passport_scan_index(lt_index, QString::number(index + 1))));
	rows.back()->hide(anim::type::instant);

	const auto scan = rows.back()->entity();

	scan->deleteClicks(
	) | rpl::start_with_next([=] {
		controller->deleteScan(type, index);
	}, scan->lifetime());

	scan->restoreClicks(
	) | rpl::start_with_next([=] {
		controller->restoreScan(type, index);
	}, scan->lifetime());

	hideError();
}

ScanButton::ScanButton(
	QWidget *parent,
	const style::PassportScanRow &st,
	const QString &name,
	const QString &status,
	bool deleted,
	bool error)
: AbstractButton(parent)
, _st(st)
, _name(
	st::passportScanNameStyle,
	name,
	Ui::NameTextOptions())
, _status(
	st::defaultTextStyle,
	status,
	Ui::NameTextOptions())
, _error(error)
, _delete(this, object_ptr<Ui::IconButton>(this, _st.remove))
, _restore(
	this,
	object_ptr<Ui::RoundButton>(
		this,
		langFactory(lng_passport_delete_scan_undo),
		_st.restore)) {
	_delete->toggle(!deleted, anim::type::instant);
	_restore->toggle(deleted, anim::type::instant);
}

void ScanButton::setImage(const QImage &image) {
	_image = image;
	update();
}

void ScanButton::setStatus(const QString &status) {
	_status.setText(
		st::defaultTextStyle,
		status,
		Ui::NameTextOptions());
	update();
}

void ScanButton::setDeleted(bool deleted) {
	_delete->toggle(!deleted, anim::type::instant);
	_restore->toggle(deleted, anim::type::instant);
	update();
}

void ScanButton::setError(bool error) {
	_error = error;
	update();
}

int ScanButton::resizeGetHeight(int newWidth) {
	_nameHeight = st::semiboldFont->height;
	_statusHeight = st::normalFont->height;
	const auto result = _st.padding.top() + _st.size + _st.padding.bottom();
	const auto right = _st.padding.right();
	_delete->moveToRight(
		right,
		(result - _delete->height()) / 2,
		newWidth);
	_restore->moveToRight(
		right,
		(result - _restore->height()) / 2,
		newWidth);
	return result + st::lineWidth;
}

int ScanButton::countAvailableWidth() const {
	return width()
		- _st.padding.left()
		- _st.textLeft
		- _st.padding.right()
		- std::max(_delete->width(), _restore->width());
}

void ScanButton::paintEvent(QPaintEvent *e) {
	Painter p(this);

	const auto left = _st.padding.left();
	const auto top = _st.padding.top();
	p.fillRect(
		left,
		height() - _st.border,
		width() - left,
		_st.border,
		_st.borderFg);

	const auto deleted = _restore->toggled();
	if (deleted) {
		p.setOpacity(st::passportScanDeletedOpacity);
	}

	if (_image.isNull()) {
		p.fillRect(left, top, _st.size, _st.size, Qt::black);
	} else {
		PainterHighQualityEnabler hq(p);
		const auto fromRect = [&] {
			if (_image.width() > _image.height()) {
				const auto shift = (_image.width() - _image.height()) / 2;
				return QRect(shift, 0, _image.height(), _image.height());
			} else {
				const auto shift = (_image.height() - _image.width()) / 2;
				return QRect(0, shift, _image.width(), _image.width());
			}
		}();
		p.drawImage(QRect(left, top, _st.size, _st.size), _image, fromRect);
	}
	const auto availableWidth = countAvailableWidth();

	p.setPen(st::windowFg);
	_name.drawLeftElided(
		p,
		left + _st.textLeft,
		top + _st.nameTop,
		availableWidth,
		width());
	p.setPen((_error && !deleted)
		? st::boxTextFgError
		: st::windowSubTextFg);
	_status.drawLeftElided(
		p,
		left + _st.textLeft,
		top + _st.statusTop,
		availableWidth,
		width());
}

EditScans::SpecialScan::SpecialScan(ScanInfo &&file)
: file(std::move(file)) {
}

EditScans::EditScans(
	QWidget *parent,
	not_null<PanelController*> controller,
	const QString &header,
	const QString &error,
	ScanListData &&scans,
	base::optional<ScanListData> &&translations)
: RpWidget(parent)
, _controller(controller)
, _error(error)
, _content(this)
, _scansList(_controller, std::move(scans))
, _translationsList(_controller, std::move(translations)) {
	setupScans(header);
}

EditScans::EditScans(
	QWidget *parent,
	not_null<PanelController*> controller,
	const QString &error,
	std::map<FileType, ScanInfo> &&specialFiles,
	base::optional<ScanListData> &&translations)
: RpWidget(parent)
, _controller(controller)
, _error(error)
, _content(this)
, _scansList(_controller)
, _translationsList(_controller, std::move(translations)) {
	setupSpecialScans(std::move(specialFiles));
}

base::optional<int> EditScans::validateGetErrorTop() {
	auto result = base::optional<int>();
	const auto suggestResult = [&](int value) {
		if (!result || *result > value) {
			result = value;
		}
	};

	if (_commonError && !somethingChanged()) {
		suggestResult(_commonError->y());
	}
	const auto suggestList = [&](FileType type) {
		auto &list = this->list(type);
		if (list.uploadMoreRequired()) {
			list.toggleError(true);
			suggestResult((list.files.size() > 5)
				? list.upload->y()
				: list.header->y());
		}
		if (const auto row = list.nonDeletedErrorRow()) {
			//toggleError(true);
			suggestResult(row->y());
		}
	};
	suggestList(FileType::Scan);
	for (const auto &[type, scan] : _specialScans) {
		if (!scan.file.key.id
			|| scan.file.deleted
			|| !scan.file.error.isEmpty()) {
			toggleSpecialScanError(type, true);
			suggestResult(scan.header->y());
		}
	}
	suggestList(FileType::Translation);
	return result;
}

EditScans::List &EditScans::list(FileType type) {
	switch (type) {
	case FileType::Scan: return _scansList;
	case FileType::Translation: return _translationsList;
	}
	Unexpected("Type in EditScans::list().");
}

const EditScans::List &EditScans::list(FileType type) const {
	switch (type) {
	case FileType::Scan: return _scansList;
	case FileType::Translation: return _translationsList;
	}
	Unexpected("Type in EditScans::list() const.");
}

void EditScans::setupScans(const QString &header) {
	const auto inner = _content.data();
	inner->move(0, 0);

	if (!_error.isEmpty()) {
		_commonError = inner->add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				inner,
				object_ptr<Ui::FlatLabel>(
					inner,
					_error,
					Ui::FlatLabel::InitType::Simple,
					st::passportVerifyErrorLabel),
				st::passportValueErrorPadding));
		_commonError->toggle(true, anim::type::instant);
	}

	setupList(inner, FileType::Scan, header);
	setupList(inner, FileType::Translation, lang(lng_passport_translation));

	init();
}

void EditScans::setupList(
		not_null<Ui::VerticalLayout*> container,
		FileType type,
		const QString &header) {
	auto &list = this->list(type);
	if (!list.initialCount) {
		return;
	}

	if (type == FileType::Scan) {
		list.divider = container->add(
			object_ptr<Ui::SlideWrap<BoxContentDivider>>(
				container,
				object_ptr<BoxContentDivider>(
					container,
					st::passportFormDividerHeight)));
		list.divider->toggle(list.files.empty(), anim::type::instant);
	}
	list.header = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				header,
				Ui::FlatLabel::InitType::Simple,
				st::passportFormHeader),
			st::passportUploadHeaderPadding));
	list.header->toggle(
		!list.divider || !list.files.empty(),
		anim::type::instant);
	if (!list.errorMissing.isEmpty()) {
		list.uploadMoreError = container->add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				container,
				object_ptr<Ui::FlatLabel>(
					container,
					list.errorMissing,
					Ui::FlatLabel::InitType::Simple,
					st::passportVerifyErrorLabel),
				st::passportUploadErrorPadding));
		list.uploadMoreError->toggle(true, anim::type::instant);
	}
	list.wrap = container->add(object_ptr<Ui::VerticalLayout>(container));
	for (const auto &scan : list.files) {
		list.pushScan(scan);
		list.rows.back()->show(anim::type::instant);
	}

	list.upload = container->add(
		object_ptr<Info::Profile::Button>(
			container,
			list.uploadTexts.events_starting_with(
				list.uploadButtonText()
			) | rpl::flatten_latest(),
			st::passportUploadButton),
		st::passportUploadButtonPadding);
	list.upload->addClickHandler([=] {
		chooseScan(type);
	});

	container->add(object_ptr<BoxContentDivider>(
		container,
		st::passportFormDividerHeight));
}

void EditScans::setupSpecialScans(std::map<FileType, ScanInfo> &&files) {
	const auto requiresBothSides = files.find(FileType::ReverseSide)
		!= end(files);
	const auto title = [&](FileType type) {
		switch (type) {
		case FileType::FrontSide:
			return lang(requiresBothSides
				? lng_passport_front_side_title
				: lng_passport_main_page_title);
		case FileType::ReverseSide:
			return lang(lng_passport_reverse_side_title);
		case FileType::Selfie:
			return lang(lng_passport_selfie_title);
		}
		Unexpected("Type in special row title.");
	};
	const auto uploadKey = [=](FileType type, bool hasScan) {
		switch (type) {
		case FileType::FrontSide:
			return requiresBothSides
				? (hasScan
					? lng_passport_reupload_front_side
					: lng_passport_upload_front_side)
				: (hasScan
					? lng_passport_reupload_main_page
					: lng_passport_upload_main_page);
		case FileType::ReverseSide:
			return hasScan
				? lng_passport_reupload_reverse_side
				: lng_passport_upload_reverse_side;
		case FileType::Selfie:
			return hasScan
				? lng_passport_reupload_selfie
				: lng_passport_upload_selfie;
		}
		Unexpected("Type in special row upload key.");
	};
	const auto description = [&](FileType type) {
		switch (type) {
		case FileType::FrontSide:
			return lang(requiresBothSides
				? lng_passport_front_side_description
				: lng_passport_main_page_description);
		case FileType::ReverseSide:
			return lang(lng_passport_reverse_side_description);
		case FileType::Selfie:
			return lang(lng_passport_selfie_description);
		}
		Unexpected("Type in special row upload key.");
	};

	const auto inner = _content.data();
	inner->move(0, 0);

	if (!_error.isEmpty()) {
		_commonError = inner->add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				inner,
				object_ptr<Ui::FlatLabel>(
					inner,
					_error,
					Ui::FlatLabel::InitType::Simple,
					st::passportVerifyErrorLabel),
				st::passportValueErrorPadding));
		_commonError->toggle(true, anim::type::instant);
	}

	for (auto &[type, info] : files) {
		const auto i = _specialScans.emplace(
			type,
			SpecialScan(std::move(info))).first;
		auto &scan = i->second;

		scan.header = inner->add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				inner,
				object_ptr<Ui::FlatLabel>(
					inner,
					title(type),
					Ui::FlatLabel::InitType::Simple,
					st::passportFormHeader),
				st::passportUploadHeaderPadding));
		scan.header->toggle(scan.file.key.id != 0, anim::type::instant);
		scan.wrap = inner->add(object_ptr<Ui::VerticalLayout>(inner));
		if (scan.file.key.id) {
			createSpecialScanRow(scan, scan.file, requiresBothSides);
		}
		auto label = scan.rowCreated.value(
		) | rpl::map([=, type = type](bool created) {
			return Lang::Viewer(uploadKey(type, created));
		}) | rpl::flatten_latest(
		) | Info::Profile::ToUpperValue();
		scan.upload = inner->add(
			object_ptr<Info::Profile::Button>(
				inner,
				std::move(label),
				st::passportUploadButton),
			st::passportUploadButtonPadding);
		scan.upload->addClickHandler([=, type = type] {
			chooseScan(type);
		});

		inner->add(object_ptr<Ui::DividerLabel>(
			inner,
			object_ptr<Ui::FlatLabel>(
				_content,
				description(type),
				Ui::FlatLabel::InitType::Simple,
				st::boxDividerLabel),
			st::passportFormLabelPadding));
	}

	setupList(inner, FileType::Translation, lang(lng_passport_translation));

	init();
}

void EditScans::init() {
	_controller->scanUpdated(
	) | rpl::start_with_next([=](ScanInfo &&info) {
		updateScan(std::move(info));
	}, lifetime());

	widthValue(
	) | rpl::start_with_next([=](int width) {
		_content->resizeToWidth(width);
	}, _content->lifetime());

	_content->heightValue(
	) | rpl::start_with_next([=](int height) {
		resize(width(), height);
	}, _content->lifetime());
}

void EditScans::updateScan(ScanInfo &&info) {
	if (info.type != FileType::Scan && info.type != FileType::Translation) {
		updateSpecialScan(std::move(info));
		return;
	}
	list(info.type).updateScan(std::move(info), width());
	updateErrorLabels();
}

void EditScans::scanFieldsChanged(bool changed) {
	if (_scanFieldsChanged != changed) {
		_scanFieldsChanged = changed;
		updateErrorLabels();
	}
}

void EditScans::updateErrorLabels() {
	const auto updateList = [&](FileType type) {
		auto &list = this->list(type);
		if (list.uploadMoreError) {
			list.uploadMoreError->toggle(
				!list.uploadedSomeMore(),
				anim::type::normal);
		}
	};
	updateList(FileType::Scan);
	updateList(FileType::Translation);
	if (_commonError) {
		_commonError->toggle(!somethingChanged(), anim::type::normal);
	}
}

bool EditScans::somethingChanged() const {
	return list(FileType::Scan).uploadedSomeMore()
		|| list(FileType::Translation).uploadedSomeMore()
		|| _scanFieldsChanged
		|| _specialScanChanged;
}

void EditScans::updateSpecialScan(ScanInfo &&info) {
	Expects(info.key.id != 0);

	const auto type = info.type;
	const auto i = _specialScans.find(type);
	if (i == end(_specialScans)) {
		return;
	}
	auto &scan = i->second;
	if (scan.file.key.id) {
		UpdateFileRow(scan.row->entity(), info);
		scan.rowCreated = !info.deleted;
		if (scan.file.key.id != info.key.id) {
			specialScanChanged(type, true);
		}
	} else {
		const auto requiresBothSides
			= (_specialScans.find(FileType::ReverseSide)
				!= end(_specialScans));
		createSpecialScanRow(scan, info, requiresBothSides);
		scan.wrap->resizeToWidth(width());
		scan.row->show(anim::type::normal);
		scan.header->show(anim::type::normal);
		specialScanChanged(type, true);
	}
	scan.file = std::move(info);
}

void EditScans::createSpecialScanRow(
		SpecialScan &scan,
		const ScanInfo &info,
		bool requiresBothSides) {
	Expects(scan.file.type != FileType::Scan
		&& scan.file.type != FileType::Translation);

	const auto type = scan.file.type;
	const auto name = [&] {
		switch (type) {
		case FileType::FrontSide:
			return lang(requiresBothSides
				? lng_passport_front_side_name
				: lng_passport_main_page_name);
		case FileType::ReverseSide:
			return lang(lng_passport_reverse_side_name);
		case FileType::Selfie:
			return lang(lng_passport_selfie_name);
		}
		Unexpected("Type in special file name.");
	}();
	scan.row = CreateScan(scan.wrap, info, name);
	const auto row = scan.row->entity();

	row->deleteClicks(
	) | rpl::start_with_next([=] {
		_controller->deleteScan(type, base::none);
	}, row->lifetime());

	row->restoreClicks(
	) | rpl::start_with_next([=] {
		_controller->restoreScan(type, base::none);
	}, row->lifetime());

	scan.rowCreated = !info.deleted;
}

void EditScans::chooseScan(FileType type) {
	if (!_controller->canAddScan(type)) {
		_controller->showToast(lang(lng_passport_scans_limit_reached));
		return;
	}
	ChooseScan(this, type, [=](QByteArray &&content) {
		_controller->uploadScan(type, std::move(content));
	}, [=](ReadScanError error) {
		_controller->readScanError(error);
	});
}

void EditScans::ChooseScan(
		QPointer<QWidget> parent,
		FileType type,
		Fn<void(QByteArray&&)> doneCallback,
		Fn<void(ReadScanError)> errorCallback) {
	Expects(parent != nullptr);

	const auto processFiles = std::make_shared<Fn<void(QStringList&&)>>();
	const auto filter = FileDialog::AllFilesFilter()
		+ qsl(";;Image files (*")
		+ cImgExtensions().join(qsl(" *"))
		+ qsl(")");
	const auto guardedCallback = crl::guard(parent, doneCallback);
	const auto guardedError = crl::guard(parent, errorCallback);
	const auto onMainCallback = [=](
			QByteArray &&content,
			QStringList &&remainingFiles) {
		crl::on_main([
			=,
			bytes = std::move(content),
			remainingFiles = std::move(remainingFiles)
		]() mutable {
			guardedCallback(std::move(bytes));
			(*processFiles)(std::move(remainingFiles));
		});
	};
	const auto onMainError = [=](ReadScanError error) {
		crl::on_main([=] {
			guardedError(error);
		});
	};
	const auto processImage = [=](
			QByteArray &&content,
			QStringList &&remainingFiles) {
		crl::async([
			=,
			bytes = std::move(content),
			remainingFiles = std::move(remainingFiles)
		]() mutable {
			auto result = ProcessImage(std::move(bytes));
			if (const auto error = base::get_if<ReadScanError>(&result)) {
				onMainError(*error);
			} else {
				auto content = base::get_if<QByteArray>(&result);
				Assert(content != nullptr);
				onMainCallback(std::move(*content), std::move(remainingFiles));
			}
		});
	};
	const auto processOpened = [=](FileDialog::OpenResult &&result) {
		if (result.paths.size() > 0) {
			(*processFiles)(std::move(result.paths));
		} else if (!result.remoteContent.isEmpty()) {
			processImage(std::move(result.remoteContent), {});
		}
	};
	*processFiles = [=](QStringList &&files) {
		while (!files.isEmpty()) {
			auto file = files.front();
			files.removeAt(0);

			auto content = [&] {
				QFile f(file);
				if (f.size() > App::kImageSizeLimit) {
					guardedError(ReadScanError::FileTooLarge);
					return QByteArray();
				} else if (!f.open(QIODevice::ReadOnly)) {
					guardedError(ReadScanError::CantReadImage);
					return QByteArray();
				}
				return f.readAll();
			}();
			if (!content.isEmpty()) {
				processImage(std::move(content), std::move(files));
				return;
			}
		}
	};
	const auto allowMany = (type == FileType::Scan)
		|| (type == FileType::Translation);
	(allowMany ? FileDialog::GetOpenPaths : FileDialog::GetOpenPath)(
		parent,
		lang(lng_passport_choose_image),
		filter,
		processOpened,
		nullptr);
}

void EditScans::hideSpecialScanError(FileType type) {
	toggleSpecialScanError(type, false);
}

void EditScans::specialScanChanged(FileType type, bool changed) {
	hideSpecialScanError(type);
	if (_specialScanChanged != changed) {
		_specialScanChanged = changed;
		updateErrorLabels();
	}
}

auto EditScans::findSpecialScan(FileType type) -> SpecialScan& {
	const auto i = _specialScans.find(type);
	Assert(i != end(_specialScans));
	return i->second;
}

void EditScans::toggleSpecialScanError(FileType type, bool shown) {
	auto &scan = findSpecialScan(type);
	if (scan.errorShown != shown) {
		scan.errorShown = shown;
		scan.errorAnimation.start(
			[=] { specialScanErrorAnimationCallback(type); },
			scan.errorShown ? 0. : 1.,
			scan.errorShown ? 1. : 0.,
			st::passportDetailsField.duration);
	}
}

void EditScans::specialScanErrorAnimationCallback(FileType type) {
	auto &scan = findSpecialScan(type);
	const auto error = scan.errorAnimation.current(
		scan.errorShown ? 1. : 0.);
	if (error == 0.) {
		scan.upload->setColorOverride(base::none);
	} else {
		scan.upload->setColorOverride(anim::color(
			st::passportUploadButton.textFg,
			st::boxTextFgError,
			error));
	}
}

EditScans::~EditScans() = default;

} // namespace Passport
