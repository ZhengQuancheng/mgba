/* Copyright (c) 2013-2019 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "FrameView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPalette>

#include <array>
#include <cmath>

#include "CoreController.h"

#include <mgba/core/core.h>
#include <mgba/feature/video-logger.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/memory.h>
#include <mgba/internal/gba/video.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/memory.h>
#endif

using namespace QGBA;

FrameView::FrameView(std::shared_ptr<CoreController> controller, QWidget* parent)
	: AssetView(controller, parent)
{
	m_ui.setupUi(this);

	m_glowTimer.setInterval(33);
	connect(&m_glowTimer, &QTimer::timeout, this, [this]() {
		++m_glowFrame;
		invalidateQueue();
	});

	m_ui.renderedView->installEventFilter(this);
	m_ui.compositedView->installEventFilter(this);

	connect(m_ui.queue, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
		Layer& layer = m_queue[item->data(Qt::UserRole).toInt()];
		layer.enabled = item->checkState() == Qt::Checked;
		if (layer.enabled) {
			m_disabled.remove(layer.id);
		} else {
			m_disabled.insert(layer.id);			
		}
		invalidateQueue();
	});
	connect(m_ui.queue, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
		if (item) {
			m_active = m_queue[item->data(Qt::UserRole).toInt()].id;
		} else {
			m_active = {};
		}
		invalidateQueue();
	});
	connect(m_ui.magnification, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this]() {
		invalidateQueue();

		QPixmap rendered = m_rendered.scaledToHeight(m_rendered.height() * m_ui.magnification->value());
		m_ui.renderedView->setPixmap(rendered);
	});
	m_controller->addFrameAction(std::bind(&FrameView::frameCallback, this, m_callbackLocker));
}

FrameView::~FrameView() {
	QMutexLocker locker(&m_mutex);
	*m_callbackLocker = false;
	if (m_vl) {
		m_vl->deinit(m_vl);
	}
}

bool FrameView::lookupLayer(const QPointF& coord, Layer*& out) {
	for (Layer& layer : m_queue) {
		if (!layer.enabled || m_disabled.contains(layer.id)) {
			continue;
		}
		QPointF location = layer.location;
		QSizeF layerDims(layer.image.width(), layer.image.height());
		QRegion region;
		if (layer.repeats) {
			if (location.x() + layerDims.width() < 0) {
				location.setX(std::fmod(location.x(), layerDims.width()));
			}
			if (location.y() + layerDims.height() < 0) {
				location.setY(std::fmod(location.y(), layerDims.height()));
			}

			region += layer.mask.translated(location.x(), location.y());
			region += layer.mask.translated(location.x() + layerDims.width(), location.y());
			region += layer.mask.translated(location.x(), location.y() + layerDims.height());
			region += layer.mask.translated(location.x() + layerDims.width(), location.y() + layerDims.height());
		} else {
			region = layer.mask.translated(location.x(), location.y());
		}

		if (region.contains(QPoint(coord.x(), coord.y()))) {
			out = &layer;
			return true;
		}
	}
	return false;
}

void FrameView::selectLayer(const QPointF& coord) {
	Layer* layer;
	if (!lookupLayer(coord, layer)) {
		return;
	}
	if (layer->id == m_active) {
		m_active = {};
	} else {
		m_active = layer->id;
	}
	m_glowFrame = 0;
}

void FrameView::disableLayer(const QPointF& coord) {
	Layer* layer;
	if (!lookupLayer(coord, layer)) {
		return;
	}
	layer->enabled = false;
	m_disabled.insert(layer->id);
}

#ifdef M_CORE_GBA
void FrameView::updateTilesGBA(bool force) {
	if (m_ui.freeze->checkState() == Qt::Checked) {
		return;
	}
	QMutexLocker locker(&m_mutex);
	m_queue.clear();
	{
		CoreController::Interrupter interrupter(m_controller);

		uint16_t* io = static_cast<GBA*>(m_controller->thread()->core->board)->memory.io;
		QRgb backdrop = M_RGB5_TO_RGB8(static_cast<GBA*>(m_controller->thread()->core->board)->video.palette[0]);
		m_gbaDispcnt = io[REG_DISPCNT >> 1];
		int mode = GBARegisterDISPCNTGetMode(m_gbaDispcnt);

		std::array<bool, 4> enabled{
			bool(GBARegisterDISPCNTIsBg0Enable(m_gbaDispcnt)),
			bool(GBARegisterDISPCNTIsBg1Enable(m_gbaDispcnt)),
			bool(GBARegisterDISPCNTIsBg2Enable(m_gbaDispcnt)),
			bool(GBARegisterDISPCNTIsBg3Enable(m_gbaDispcnt)),
		};

		for (int priority = 0; priority < 4; ++priority) {
			for (int sprite = 0; sprite < 128; ++sprite) {
				ObjInfo info;
				lookupObj(sprite, &info);

				if (!info.enabled || info.priority != priority) {
					continue;
				}

				QPointF offset(info.x, info.y);
				QImage obj(compositeObj(info));
				if (info.hflip || info.vflip) {
					obj = obj.mirrored(info.hflip, info.vflip);
				}
				m_queue.append({
					{ LayerId::SPRITE, sprite },
					!m_disabled.contains({ LayerId::SPRITE, sprite }),
					QPixmap::fromImage(obj),
					{}, offset, false
				});
				if (m_queue.back().image.hasAlpha()) {
					m_queue.back().mask = QRegion(m_queue.back().image.mask());
				} else {
					m_queue.back().mask = QRegion(0, 0, m_queue.back().image.width(), m_queue.back().image.height());
				}
			}

			for (int bg = 0; bg < 4; ++bg) {
				if (!enabled[bg]) {
					continue;
				}
				if (GBARegisterBGCNTGetPriority(io[(REG_BG0CNT >> 1) + bg]) != priority) {
					continue;
				}

				QPointF offset;
				if (mode == 0) {
					offset.setX(-(io[(REG_BG0HOFS >> 1) + (bg << 1)] & 0x1FF));
					offset.setY(-(io[(REG_BG0VOFS >> 1) + (bg << 1)] & 0x1FF));
				};
				m_queue.append({
					{ LayerId::BACKGROUND, bg },
					!m_disabled.contains({ LayerId::BACKGROUND, bg }),
					QPixmap::fromImage(compositeMap(bg, m_mapStatus[bg])),
					{}, offset, true
				});
				if (m_queue.back().image.hasAlpha()) {
					m_queue.back().mask = QRegion(m_queue.back().image.mask());
				} else {
					m_queue.back().mask = QRegion(0, 0, m_queue.back().image.width(), m_queue.back().image.height());
				}
			}
		}
		QImage backdropImage(QSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS), QImage::Format_Mono);
		backdropImage.fill(1);
		backdropImage.setColorTable({backdrop, backdrop | 0xFF000000 });
		m_queue.append({
			{ LayerId::BACKDROP },
			!m_disabled.contains({ LayerId::BACKDROP }),
			QPixmap::fromImage(backdropImage),
			{}, {0, 0}, false
		});
		updateRendered();
	}
	invalidateQueue(QSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS));
}

void FrameView::injectGBA() {
	mVideoLogger* logger = m_vl->videoLogger;
	mVideoLoggerInjectionPoint(logger, LOGGER_INJECTION_FIRST_SCANLINE);
	GBA* gba = static_cast<GBA*>(m_vl->board);
	gba->video.renderer->highlightBG[0] = false;
	gba->video.renderer->highlightBG[1] = false;
	gba->video.renderer->highlightBG[2] = false;
	gba->video.renderer->highlightBG[3] = false;
	for (int i = 0; i < 128; ++i) {
		gba->video.renderer->highlightOBJ[i] = false;
	}
	QPalette palette;
	gba->video.renderer->highlightColor = palette.color(QPalette::HighlightedText).rgb();
	gba->video.renderer->highlightAmount = sin(m_glowFrame * M_PI / 30) * 64 + 64;

	for (const Layer& layer : m_queue) {
		switch (layer.id.type) {
		case LayerId::SPRITE:
			if (!layer.enabled) {
				mVideoLoggerInjectOAM(logger, layer.id.index << 2, 0x200);
			}
			if (layer.id == m_active) {
				gba->video.renderer->highlightOBJ[layer.id.index] = true;
			}
			break;
		case LayerId::BACKGROUND:
			m_vl->enableVideoLayer(m_vl, layer.id.index, layer.enabled);
			if (layer.id == m_active) {
				gba->video.renderer->highlightBG[layer.id.index] = true;
			}
			break;
		}
	}
	if (m_ui.disableScanline->checkState() == Qt::Checked) {
		mVideoLoggerIgnoreAfterInjection(logger, (1 << DIRTY_PALETTE) | (1 << DIRTY_OAM) | (1 << DIRTY_REGISTER));
	} else {
		mVideoLoggerIgnoreAfterInjection(logger, 0);		
	}
}
#endif

#ifdef M_CORE_GB
void FrameView::updateTilesGB(bool force) {
	if (m_ui.freeze->checkState() == Qt::Checked) {
		return;
	}
	m_queue.clear();
	{
		CoreController::Interrupter interrupter(m_controller);
		updateRendered();
	}
	invalidateQueue(m_controller->screenDimensions());
}

void FrameView::injectGB() {
	for (const Layer& layer : m_queue) {
	}
}
#endif

void FrameView::invalidateQueue(const QSize& dims) {
	if (dims.isValid()) {
		m_dims = dims;
	}
	bool blockSignals = m_ui.queue->blockSignals(true);
	QMutexLocker locker(&m_mutex);
	if (m_vl) {
		m_vl->reset(m_vl);
		switch (m_controller->platform()) {
#ifdef M_CORE_GBA
		case PLATFORM_GBA:
			injectGBA();
#endif
#ifdef M_CORE_GB
		case PLATFORM_GB:
			injectGB();
#endif
		}
		m_vl->runFrame(m_vl);
	}

	for (int i = 0; i < m_queue.count(); ++i) {
		const Layer& layer = m_queue[i];
		QListWidgetItem* item;
		if (i >= m_ui.queue->count()) {
			item = new QListWidgetItem;
			m_ui.queue->addItem(item);
		} else {
			item = m_ui.queue->item(i);
		}
		item->setText(layer.id.readable());
		item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		item->setCheckState(layer.enabled ? Qt::Checked : Qt::Unchecked);
		item->setData(Qt::UserRole, i);
		item->setSelected(layer.id == m_active);
	}

	while (m_ui.queue->count() > m_queue.count()) {
		delete m_ui.queue->takeItem(m_queue.count());
	}
	m_ui.queue->blockSignals(blockSignals);

	QPixmap composited;
	if (m_framebuffer.isNull()) {
		updateRendered();
		composited = m_rendered;
	} else {
		composited.convertFromImage(m_framebuffer);
	}
	m_composited = composited.scaled(m_dims * m_ui.magnification->value());
	m_ui.compositedView->setPixmap(m_composited);
}

void FrameView::updateRendered() {
	if (m_ui.freeze->checkState() == Qt::Checked) {
		return;
	}
	m_rendered.convertFromImage(m_controller->getPixels());
	QPixmap rendered = m_rendered.scaledToHeight(m_rendered.height() * m_ui.magnification->value());
	m_ui.renderedView->setPixmap(rendered);
}

bool FrameView::eventFilter(QObject* obj, QEvent* event) {
	QPointF pos;
	switch (event->type()) {
	case QEvent::MouseButtonPress:
		pos = static_cast<QMouseEvent*>(event)->localPos();
		pos /= m_ui.magnification->value();
		selectLayer(pos);
		return true;
	case QEvent::MouseButtonDblClick:
		pos = static_cast<QMouseEvent*>(event)->localPos();
		pos /= m_ui.magnification->value();
		disableLayer(pos);
		return true;
	}
	return false;
}

void FrameView::refreshVl() {
	QMutexLocker locker(&m_mutex);
	m_currentFrame = m_nextFrame;
	m_nextFrame = VFileMemChunk(nullptr, 0);
	if (m_currentFrame) {
		m_controller->endVideoLog(false);
		VFile* currentFrame = VFileMemChunk(nullptr, m_currentFrame->size(m_currentFrame));
		void* buffer = currentFrame->map(currentFrame, m_currentFrame->size(m_currentFrame), MAP_WRITE);
		m_currentFrame->seek(m_currentFrame, 0, SEEK_SET);
		m_currentFrame->read(m_currentFrame, buffer, m_currentFrame->size(m_currentFrame));
		currentFrame->unmap(currentFrame, buffer, m_currentFrame->size(m_currentFrame));
		m_currentFrame = currentFrame;
		QMetaObject::invokeMethod(this, "newVl");
	}
	m_controller->endVideoLog();
	m_controller->startVideoLog(m_nextFrame, false);
}

void FrameView::newVl() {
	if (!m_glowTimer.isActive()) {
		m_glowTimer.start();
	}
	QMutexLocker locker(&m_mutex);
	if (m_vl) {
		m_vl->deinit(m_vl);
	}
	m_vl = mCoreFindVF(m_currentFrame);
	m_vl->init(m_vl);
	m_vl->loadROM(m_vl, m_currentFrame);
	mCoreInitConfig(m_vl, nullptr);
	unsigned width, height;
	m_vl->desiredVideoDimensions(m_vl, &width, &height);
	m_framebuffer = QImage(width, height, QImage::Format_RGBX8888);
	m_vl->setVideoBuffer(m_vl, reinterpret_cast<color_t*>(m_framebuffer.bits()), width);
	m_vl->reset(m_vl);
}

void FrameView::frameCallback(FrameView* viewer, std::shared_ptr<bool> lock) {
	if (!*lock) {
		return;
	}
	CoreController::Interrupter interrupter(viewer->m_controller, true);
	viewer->refreshVl();
	viewer->m_controller->addFrameAction(std::bind(&FrameView::frameCallback, viewer, lock));
}


QString FrameView::LayerId::readable() const {
	QString typeStr;
	switch (type) {
	case NONE:
		return tr("None");
	case BACKGROUND:
		typeStr = tr("Background");
		break;
	case WINDOW:
		typeStr = tr("Window");
		break;
	case SPRITE:
		typeStr = tr("Sprite");
		break;
	case BACKDROP:
		typeStr = tr("Backdrop");
		break;
	}
	if (index < 0) {
		return typeStr;
	}
	return tr("%1 %2").arg(typeStr).arg(index);
}