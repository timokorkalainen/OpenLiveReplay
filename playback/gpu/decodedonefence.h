#ifndef OLR_DECODEDONEFENCE_H
#define OLR_DECODEDONEFENCE_H

#include <memory>

class DecodeDoneFence {
public:
    static std::shared_ptr<DecodeDoneFence> create();
    virtual ~DecodeDoneFence();

    virtual void signalDecodeDone() = 0;
    virtual bool waitDecodeDone(int timeoutMs) = 0;
    virtual bool isSignaled() const = 0;
};

#endif // OLR_DECODEDONEFENCE_H
