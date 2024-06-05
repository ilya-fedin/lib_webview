// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_compositor.h"

#include "base/flat_map.h"
#include "base/unique_qptr.h"
#include "base/qt_signal_producer.h"
#include "base/event_filter.h"

#include <QtQuickWidgets/QQuickWidget>
#include <QtWaylandCompositor/QWaylandXdgSurface>
#include <QtWaylandCompositor/QWaylandXdgOutputV1>
#include <QtWaylandCompositor/QWaylandQuickOutput>
#include <QtWaylandCompositor/QWaylandQuickShellSurfaceItem>

#include <crl/crl.h>

namespace Webview {

struct Compositor::Private {
	Private(Compositor *parent)
	: shell(parent)
	, xdgOutput(parent) {
	}

	QPointer<QQuickWidget> widget;
	base::unique_qptr<Output> output;
	QWaylandXdgShell shell;
	QWaylandXdgOutputManagerV1 xdgOutput;
	rpl::lifetime lifetime;
};

class Compositor::Chrome : public QWaylandQuickShellSurfaceItem {
public:
	Chrome(
		Output *output,
		QQuickWindow *window,
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize);

	rpl::producer<> surfaceCompleted() const {
		return _completed.value()
			| rpl::filter(rpl::mappers::_1)
			| rpl::to_empty;
	}

private:
	QWaylandSurface _surfaceProxy;
	QWaylandXdgSurface _xdgSurfaceProxy;
	rpl::event_stream<> _xdgToplevelTitleChangedProxy;
	rpl::event_stream<> _xdgToplevelFullscreenChangedProxy;
	QQuickItem _moveItem;
	rpl::variable<bool> _completed = false;
	rpl::lifetime _lifetime;
};

class Compositor::Output : public QWaylandQuickOutput {
public:
	Output(
			Compositor *compositor,
			QQuickWindow *window,
			QWaylandXdgSurface *xdgSurface = nullptr)
	: _xdg(this, &compositor->_private->xdgOutput)
	, _windowFollowsSize(xdgSurface) {
		connect(window, &QObject::destroyed, this, [=] { delete this; });
		setCompositor(compositor);
		setWindow(window);
		setScaleFactor(this->window()->devicePixelRatio());
		setSizeFollowsWindow(true);
		this->window()->setProperty("output", QVariant::fromValue(this));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
		const auto guard = QPointer(this);
		crl::on_main(this, [=] {
			base::install_event_filter(this->window(), [=](
					not_null<QEvent*> e) {
				if (e->type() == QEvent::DevicePixelRatioChange) {
					QMetaObject::invokeMethod(this, [=] {
						if (!guard) {
							return;
						}
						setScaleFactor(this->window()->devicePixelRatio());
					});
				}
				return base::EventFilterResult::Continue;
			});
		});
#endif // Qt >= 6.6.0
		rpl::single(rpl::empty) | rpl::then(
			rpl::merge(
				base::qt_signal_producer(
					this,
					&QWaylandOutput::geometryChanged
				),
				base::qt_signal_producer(
					this,
					&QWaylandOutput::scaleFactorChanged
				)
			)
		) | rpl::map([=] {
			return std::make_tuple(geometry(), scaleFactor());
		}) | rpl::start_with_next([=](QRect geometry, int scaleFactor) {
			_xdg.setLogicalPosition(geometry.topLeft() / scaleFactor);
			_xdg.setLogicalSize(geometry.size() / scaleFactor);
		}, _lifetime);
		setXdgSurface(xdgSurface);
	}

	QQuickWindow *window() const {
		return static_cast<QQuickWindow*>(QWaylandQuickOutput::window());
	}

	Chrome *chrome() const {
		return _chrome;
	}

	void setXdgSurface(QPointer<QWaylandXdgSurface> xdgSurface) {
		crl::on_main(this, [=] {
			if (xdgSurface) {
				_chrome.emplace(
					this,
					window(),
					xdgSurface,
					_windowFollowsSize);
			} else {
				_chrome.reset();
			}
		});
	}

private:
	QWaylandXdgOutputV1 _xdg;
	const bool _windowFollowsSize = false;
	base::unique_qptr<Chrome> _chrome;
	rpl::lifetime _lifetime;
};

Compositor::Chrome::Chrome(
		Output *output,
		QQuickWindow *window,
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize)
: QWaylandQuickShellSurfaceItem(window->contentItem()) {
	connect(xdgSurface, &QObject::destroyed, this, [=] { delete this; });

	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			view(),
			&QWaylandView::surfaceChanged
		)
	) | rpl::start_with_next([=] {
		setOutput(output);
	}, _lifetime);

	setShellSurface(xdgSurface);
	setAutoCreatePopupItems(false);
	setMoveItem(&_moveItem);
	_moveItem.setEnabled(false);
	xdgSurface->setProperty("window", QVariant::fromValue(window));

	base::install_event_filter(this, window, [=](not_null<QEvent*> e) {
		if (e->type() != QEvent::Close) {
			return base::EventFilterResult::Continue;
		}
		e->ignore();
		QMetaObject::invokeMethod(xdgSurface, [=] {
			if (const auto toplevel = xdgSurface->toplevel()) {
				toplevel->sendClose();
			} else if (const auto popup = xdgSurface->popup()) {
				popup->sendPopupDone();
			}
		});
		return base::EventFilterResult::Cancel;
	});

	rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				window,
				&QWindow::widthChanged
			),
			base::qt_signal_producer(
				window,
				&QWindow::heightChanged
			)
		) | rpl::to_empty
	) | rpl::map([=] {
		return window->size();
	}) | rpl::distinct_until_changed(
	) | rpl::filter([=](const QSize &size) {
		return !size.isEmpty();
	}) | rpl::start_with_next([=](const QSize &size) {
		QMetaObject::invokeMethod(xdgSurface, [=] {
			if (const auto toplevel = xdgSurface->toplevel()) {
				toplevel->sendFullscreen(size);
			}
		});
	}, _lifetime);

	connect(
		xdgSurface->surface(),
		&QWaylandSurface::destinationSizeChanged,
		&_surfaceProxy,
		&QWaylandSurface::destinationSizeChanged);

	connect(
		xdgSurface,
		&QWaylandXdgSurface::windowGeometryChanged,
		&_xdgSurfaceProxy,
		&QWaylandXdgSurface::windowGeometryChanged);

	rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				&_surfaceProxy,
				&QWaylandSurface::destinationSizeChanged
			),
			base::qt_signal_producer(
				&_xdgSurfaceProxy,
				&QWaylandXdgSurface::windowGeometryChanged
			)
		)
	) | rpl::map([=] {
		return xdgSurface->windowGeometry().isValid()
			? xdgSurface->windowGeometry()
			: QRect(QPoint(), xdgSurface->surface()->destinationSize());
	}) | rpl::distinct_until_changed(
	) | rpl::filter([=](const QRect &geometry) {
		return geometry.isValid();
	}) | rpl::start_with_next([=](const QRect &geometry) {
		setX(-geometry.x());
		setY(-geometry.y());

		if (windowFollowsSize) {
			if (xdgSurface->popup()) {
				window->setMinimumSize(geometry.size());
				window->setMaximumSize(geometry.size());
			} else {
				window->resize(geometry.size());
			}
		}

		_completed = true;
	}, _lifetime);

	if (const auto toplevel = xdgSurface->toplevel()) {
		connect(
			toplevel,
			&QWaylandXdgToplevel::titleChanged,
			this,
			[=] {
				_xdgToplevelTitleChangedProxy.fire({});
			});

		rpl::single(rpl::empty) | rpl::then(
			_xdgToplevelTitleChangedProxy.events()
		) | rpl::map([=] {
			return toplevel->title();
		}) | rpl::start_with_next([=](const QString &title) {
			window->setTitle(title);
		}, _lifetime);

		connect(
			toplevel,
			&QWaylandXdgToplevel::fullscreenChanged,
			this,
			[=] {
				_xdgToplevelFullscreenChangedProxy.fire({});
			});

		rpl::single(rpl::empty) | rpl::then(
			_xdgToplevelFullscreenChangedProxy.events()
		) | rpl::map([=] {
			return toplevel->fullscreen();
		}) | rpl::start_with_next([=](bool fullscreen) {
			QMetaObject::invokeMethod(toplevel, [=] {
				if (!fullscreen) {
					toplevel->sendFullscreen(window->size());
				}
			});
		}, _lifetime);
	}
}

Compositor::Compositor(const QByteArray &socketName)
: _private(std::make_unique<Private>(this)) {
	connect(&_private->shell, &QWaylandXdgShell::toplevelCreated, [=](
			QWaylandXdgToplevel *toplevel,
			QWaylandXdgSurface *xdgSurface) {
		if (!_private->output || _private->output->chrome()) {
			crl::on_main([=] {
				const auto window = new QQuickWindow;
				connect(xdgSurface, &QObject::destroyed, window, [=] {
					delete window;
				});

				QMetaObject::invokeMethod(xdgSurface, [=] {
					const auto output = new Output(this, window, xdgSurface);
					crl::on_main(output, [=] {
						output->chrome()->surfaceCompleted(
						) | rpl::start_with_next([=] {
							window->show();
						}, _private->lifetime);
					});
				});
			});
		} else {
			_private->output->setXdgSurface(xdgSurface);
		}
	});

	connect(&_private->shell, &QWaylandXdgShell::popupCreated, [=](
			QWaylandXdgPopup *popup,
			QWaylandXdgSurface *xdgSurface) {
		crl::on_main(xdgSurface, [=] {
			const auto widget = _private->widget;
			const auto parent = (*static_cast<QQuickWindow * const *>(
				popup->parentXdgSurface()->property("window").constData()
			));
			const auto output = (*static_cast<Output * const *>(
				parent->property("output").constData()
			));
			const auto window = new QQuickWindow;
			connect(xdgSurface, &QObject::destroyed, window, [=] {
				delete window;
			});
			window->setProperty("output", QVariant::fromValue(output));
			const auto chrome = new Chrome(output, window, xdgSurface, true);

			chrome->surfaceCompleted() | rpl::start_with_next([=] {
				if (widget && parent == widget->quickWindow()) {
					window->setTransientParent(
						widget->window()->windowHandle());
					window->setPosition(
						popup->unconstrainedPosition()
							+ widget->mapToGlobal(QPoint()));
				} else {
					window->setTransientParent(parent);
					window->setPosition(
						popup->unconstrainedPosition() + parent->position());
				}
				window->setFlag(Qt::Popup);
				window->setColor(Qt::transparent);
				window->show();
			}, _private->lifetime);
		});
	});

	setSocketName(socketName);
	create();
}

Compositor::~Compositor() = default;

void Compositor::setWidget(QQuickWidget *widget) {
	_private->widget = widget;
	if (widget) {
		_private->output.emplace(this, widget->quickWindow());
	} else {
		_private->output.reset();
	}
}

} // namespace Webview
