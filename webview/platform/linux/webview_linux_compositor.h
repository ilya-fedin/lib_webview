// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QThread>
#include <QtWaylandCompositor/QWaylandQuickCompositor>

class QQuickWidget;

namespace Webview {

class Compositor : public QWaylandQuickCompositor {
public:
	Compositor(const QByteArray &socketName = {});
	~Compositor();

	void setWidget(QQuickWidget *widget);

private:
	class Output;
	class Chrome;

	struct Private;
	const std::unique_ptr<Private> _private;
};

class CompositorThread : public QThread {
public:
	template <typename ...Args>
	CompositorThread(Args... args) {
		connect(this, &QThread::started, [=] {
			_compositor.emplace(args...);
		});

		connect(this, &QThread::finished, [=] {
			_compositor.reset();
		});

		start();
	}

	~CompositorThread() {
		quit();
		wait();
	}

	[[nodiscard]] Compositor &get() {
		return *_compositor;
	}

private:
	std::optional<Compositor> _compositor;

};

} // namespace Webview
