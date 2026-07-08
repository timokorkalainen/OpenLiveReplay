#ifndef FRAMEPREVIEWITEM_H
#define FRAMEPREVIEWITEM_H

#include "playback/frameprovider.h"

#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QQuickPaintedItem>

class FramePreviewItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(FrameProvider* provider READ provider WRITE setProvider NOTIFY providerChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit FramePreviewItem(QQuickItem* parent = nullptr);
    ~FramePreviewItem() override;

    FrameProvider* provider() const { return m_provider; }
    void setProvider(FrameProvider* provider);
    bool active() const { return m_active; }
    void setActive(bool active);

    void paint(QPainter* painter) override;

signals:
    void providerChanged();
    void activeChanged();

private:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void unregisterConsumer();
    void updateConsumerRegistration();
    void updateCachedImage(quint64 requestedSerial = 0);

    QPointer<FrameProvider> m_provider;
    QPointer<FrameProvider> m_registeredProvider;
    QMetaObject::Connection m_frameChangedConnection;
    mutable QMutex m_imageMutex;
    QImage m_cachedImage;
    quint64 m_cachedSerial = 0;
    bool m_active = false;
};

#endif // FRAMEPREVIEWITEM_H
