#include "playback/framepreviewitem.h"

#include <QPainter>

FramePreviewItem::FramePreviewItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setFillColor(Qt::black);
}

FramePreviewItem::~FramePreviewItem() {
    if (m_frameChangedConnection) QObject::disconnect(m_frameChangedConnection);
    unregisterConsumer();
}

void FramePreviewItem::setProvider(FrameProvider* provider) {
    if (m_provider == provider) return;

    unregisterConsumer();
    if (m_frameChangedConnection) {
        QObject::disconnect(m_frameChangedConnection);
        m_frameChangedConnection = {};
    }
    m_provider = provider;
    if (m_provider) {
        m_frameChangedConnection = QObject::connect(
            m_provider, &FrameProvider::frameChanged, this,
            [this](quint64 serial) { updateCachedImage(serial); }, Qt::QueuedConnection);
    }
    updateCachedImage();
    updateConsumerRegistration();
    emit providerChanged();
}

void FramePreviewItem::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    updateConsumerRegistration();
    if (m_active) updateCachedImage();
    emit activeChanged();
}

void FramePreviewItem::paint(QPainter* painter) {
    if (!painter) return;

    painter->fillRect(boundingRect(), Qt::black);

    QImage image;
    quint64 serial = 0;
    {
        QMutexLocker locker(&m_imageMutex);
        image = m_cachedImage;
        serial = m_cachedSerial;
    }
    if (image.isNull() || width() <= 0 || height() <= 0) {
        if (m_registeredProvider && serial > 0)
            m_registeredProvider->markDirectPreviewConsumerPainted(this, serial);
        return;
    }

    const QSizeF sourceSize(image.width(), image.height());
    QSizeF targetSize = sourceSize;
    targetSize.scale(boundingRect().size(), Qt::KeepAspectRatio);
    const QPointF topLeft((width() - targetSize.width()) / 2.0,
                          (height() - targetSize.height()) / 2.0);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(QRectF(topLeft, targetSize), image);
    if (m_registeredProvider && serial > 0)
        m_registeredProvider->markDirectPreviewConsumerPainted(this, serial);
}

void FramePreviewItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemSceneChange || change == ItemVisibleHasChanged) {
        updateConsumerRegistration();
    }
}

void FramePreviewItem::unregisterConsumer() {
    if (!m_registeredProvider) return;
    m_registeredProvider->removeDirectPreviewConsumer(this);
    m_registeredProvider = nullptr;
}

void FramePreviewItem::updateConsumerRegistration() {
    FrameProvider* desiredProvider =
        (m_active && isVisible() && window() && m_provider) ? m_provider.data() : nullptr;
    if (m_registeredProvider == desiredProvider) return;

    unregisterConsumer();
    if (desiredProvider) {
        desiredProvider->addDirectPreviewConsumer(this);
        m_registeredProvider = desiredProvider;
    }
}

void FramePreviewItem::updateCachedImage(quint64 requestedSerial) {
    quint64 serial = 0;
    QImage image = m_provider ? m_provider->latestImage(&serial) : QImage();
    if (requestedSerial > 0 && serial < requestedSerial) return;
    {
        QMutexLocker locker(&m_imageMutex);
        m_cachedImage = std::move(image);
        m_cachedSerial = m_cachedImage.isNull() ? 0 : serial;
    }
    update();
}
