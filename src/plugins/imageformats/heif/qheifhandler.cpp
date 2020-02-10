/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the HEIF plugins in the Qt ImageFormats module.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qheifhandler_p.h"

#include <QtGui/QImage>
#include <QtCore/QSize>
#include <QtCore/QVariant>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

constexpr int kDefaultQuality = 50;  // TODO: maybe adjust this
constexpr int kHeaderSize = 12;

QHeifHandler::QHeifHandler() :
    QImageIOHandler(),
    _device{nullptr},
    _readState{nullptr},
    _quality{kDefaultQuality}
{
    qDebug("QHeifHandler!");
}

QHeifHandler::~QHeifHandler()
{
}

void QHeifHandler::updateDevice()
{
    // !_device ==> !_readState
    Q_ASSERT(_device || !_readState);

    if (!device()) {
        qWarning("QHeifHandler::updateDevice() device is null");
    }

    if (device() != _device) {
        // new device; re-read data
        _device = device();
        _readState.reset();
    }
}

bool QHeifHandler::canReadFrom(QIODevice& device)
{
    QByteArray header = device.peek(kHeaderSize);
    if (header.size() != kHeaderSize) {
        return false;
    }

    auto result = heif_check_filetype(reinterpret_cast<const uint8_t*>(header.constData()),
                                      header.size());
    return result == heif_filetype_yes_supported || result == heif_filetype_maybe;
}

bool QHeifHandler::canRead() const
{
    if (!device() || !canReadFrom(*device())) {
        return false;
    }

    // Other image plugins set the format here. Not sure if it is really
    // necessary or what it accomplishes.
    QByteArray header = device()->peek(kHeaderSize);
    QLatin1String mimeType(heif_get_file_mime_type(reinterpret_cast<const uint8_t*>(header.constData()),
                                                   header.size()));
    if (mimeType == QLatin1String("image/heic")) {
        setFormat("heic");
    } else if (mimeType == QLatin1String("image/heif")) {
        setFormat("heif");
    } else if (mimeType == QLatin1String("image/heic-sequence")) {
        setFormat("heics");
    } else if (mimeType == QLatin1String("image/heif-sequence")) {
        setFormat("heifs");
    }

    return true;
}

namespace {

static_assert(heif_error_Ok == 0, "heif_error_Ok assumed to be 0");

template<class T, class D>
std::unique_ptr<T, D> wrapPointer(T* ptr, D deleter)
{
    return std::unique_ptr<T, D>(ptr, deleter);
}

int64_t qheif_reader_get_position(void* userdata)
{
    QIODevice* device = reinterpret_cast<QIODevice*>(userdata);
    return device->pos();
}

int qheif_reader_read(void* data, size_t size, void* userdata)
{
    QIODevice* device = reinterpret_cast<QIODevice*>(userdata);
    char* charData = reinterpret_cast<char*>(data);
    while (size) {
        // TODO: is this safe use of size_t?
        qint64 rd = device->read(charData, size);
        if (rd < 0) {
            return rd;
        } else if (static_cast<size_t>(rd) < size) {
            size -= rd;
            charData += rd;
            continue;
        }
        break;
    }
    return 0;
}

int qheif_reader_seek(int64_t pos, void* userdata)
{
    QIODevice* device = reinterpret_cast<QIODevice*>(userdata);
    if (device->isSequential()) {
        return -1;
    }
    return device->seek(pos) ? 0 : -1;
}

// TODO: It seems like seeking is mandatory, so sequential devices would have to have some kind of buffer.
// Do something about that.

heif_reader_grow_status qheif_reader_wait_for_file_size(int64_t target_size, void* userdata)
{
    QIODevice* device = reinterpret_cast<QIODevice*>(userdata);
    if (device->size() >= target_size) {
        return heif_reader_grow_status_size_reached;
    } else {
        return heif_reader_grow_status_size_beyond_eof;
    }
}

static struct heif_reader qheif_reader = {
    .reader_api_version = 1,
    .get_position = qheif_reader_get_position,
    .read = qheif_reader_read,
    .seek = qheif_reader_seek,
    .wait_for_file_size = qheif_reader_wait_for_file_size
};

}  // namespace

QHeifHandler::ReadState::ReadState(std::shared_ptr<heif_context>&& ctx,
                                   std::vector<heif_item_id>&& ids,
                                   int primaryIndex)
    : context(std::move(ctx))
    , idList(std::move(ids))
{
    setCurrentIndex(primaryIndex);
}

bool QHeifHandler::ReadState::setCurrentIndex(int index)
{
    if (currentIndex == index) {
        return true;
    } else if (index < 0 || index >= idList.size()) {
        return false;
    }
    currentIndex = index;

    heif_image_handle* handlePtr = nullptr;
    auto error = heif_context_get_image_handle(context.get(), idList[index], &handlePtr);
    imageHandle = std::shared_ptr<heif_image_handle>(handlePtr, heif_image_handle_release);
    if (error.code || !imageHandle) {
        qDebug("QHeifHandler::read() failed to get image handle: %s", error.message);
        return false;
    }
    return true;
}

void QHeifHandler::loadContext()
{
    updateDevice();

    if (!device()) {
        return;
    }

    if (_readState) {
        // context already loaded
        return;
    }

    // set up new context
    std::shared_ptr<heif_context> context(heif_context_alloc(), heif_context_free);
    if (!context) {
        qDebug("QHeifHandler::loadContext() failed to alloc context");
        return;
    }

    auto error = heif_context_read_from_reader(context.get(), &qheif_reader,
                                               reinterpret_cast<void*>(device()),
                                               nullptr);
    if (error.code) {
        qDebug("QHeifHandler::loadContext() failed to read context: %s", error.message);
        return;
    }

    int numImages = heif_context_get_number_of_top_level_images(context.get());
    std::vector<heif_item_id> idList(numImages, 0);
    int numIdsStored = heif_context_get_list_of_top_level_image_IDs(context.get(),
                                                                    idList.data(),
                                                                    numImages);
    Q_UNUSED(numIdsStored);
    Q_ASSERT(numIdsStored == numImages);

    // find primary image in sequence; no ordering guaranteed for id values
    heif_item_id id{};
    error = heif_context_get_primary_image_ID(context.get(), &id);
    if (error.code) {
        qDebug("QHeifHandler::loadContext() failed to get primary ID: %s", error.message);
        return;
    }

    auto iter = std::find(idList.begin(), idList.end(), id);
    if (iter == idList.end()) {
        qDebug("QHeifHandler::loadContext() primary image not found in id list");
        return;
    }

    int primaryIndex = static_cast<int>(iter - idList.begin());
    _readState.reset(new ReadState{std::move(context), std::move(idList), primaryIndex});
}

bool QHeifHandler::read(QImage* destImage)
{
    if (!destImage) {
        qWarning("QHeifHandler::read() QImage to read into is null");
        return false;
    }

    loadContext();
    if (!_readState || !_readState->imageHandle) {
        qWarning("QHeifHandler::read() failed to create context");
        return false;
    }

    // decode image
    heif_image* srcImagePtr = nullptr;
    auto error = heif_decode_image(_readState->imageHandle.get(),
                              &srcImagePtr,
                              heif_colorspace_RGB,
                              heif_chroma_interleaved_RGBA,
                              nullptr);

    auto srcImage = wrapPointer(srcImagePtr, heif_image_release);
    if (error.code || !srcImage) {
        qDebug("QHeifHandler::read() failed to decode image: %s", error.message);
        return false;
    }

    auto channel = heif_channel_interleaved;
    QSize imgSize(heif_image_get_width(srcImage.get(), channel),
                  heif_image_get_height(srcImage.get(), channel));

    if (!imgSize.isValid()) {
        qWarning("QHeifHandler::read() invalid image size: %d x %d",
                 imgSize.width(), imgSize.height());
        return false;
    }

    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(srcImage.get(), channel, &stride);

    if (!data) {
        qWarning("QHeifHandler::read() pixel data not found");
        return false;
    }

    if (stride <= 0) {
        qWarning("QHeifHandler::read() invalid stride: %d", stride);
        return false;
    }

    // move data ownership to QImage
    heif_image* dataImage = srcImage.release();

    *destImage = QImage(
        data, imgSize.width(), imgSize.height(),
        stride, QImage::Format_RGBA8888,
        [](void* img) { heif_image_release(static_cast<heif_image*>(img)); },
        dataImage
    );

    return true;
}

int QHeifHandler::currentImageNumber() const
{
    if (!_readState) {
        return -1;
    }

    return _readState->currentIndex;
}

int QHeifHandler::imageCount() const
{
    if (!_readState) {
        return 0;
    }

    return static_cast<int>(_readState->idList.size());
}

bool QHeifHandler::jumpToImage(int index)
{
    if (!_readState) {
        return false;
    }

    return _readState->setCurrentIndex(index);
}

bool QHeifHandler::jumpToNextImage()
{
    if (!_readState) {
        return false;
    }

    return jumpToImage(_readState->currentIndex + 1);
}

namespace {

#if LIBHEIF_NUMERIC_VERSION >= 0x01020000
constexpr auto kWriteErrorCode = heif_error_Encoding_error;
constexpr auto kWriteSubErrorCode = heif_suberror_Cannot_write_output_data;
#else
constexpr auto kWriteErrorCode = heif_error_Usage_error;
constexpr auto kWriteSubErrorCode = heif_suberror_Unsupported_parameter;
#endif

heif_error handleWrite(heif_context* ctx, const void* data, size_t size, void* device)
{
    Q_UNUSED(ctx);
    Q_ASSERT(data && device);

    using I = typename std::conditional<sizeof(size_t) >= sizeof(qint64),
                                        size_t,
                                        qint64>::type;

    if (static_cast<I>(size) > static_cast<I>(std::numeric_limits<qint64>::max())) {
        return {kWriteErrorCode, kWriteSubErrorCode, "size too big"};
    }

    qint64 bytesWritten = static_cast<QIODevice*>(device)->write(
        static_cast<const char*>(data), static_cast<qint64>(size));

    if (bytesWritten != static_cast<qint64>(size)) {
        return {kWriteErrorCode, kWriteSubErrorCode, "not all data written"};
    }

    return {heif_error_Ok, heif_suberror_Unspecified, "ok"};
}

}  // namespace

bool QHeifHandler::write(const QImage& preConvSrcImage)
{
    updateDevice();

    if (!device()) {
        qWarning("QHeifHandler::write() device null before write");
        return false;
    }

    if (preConvSrcImage.isNull()) {
        qWarning("QHeifHandler::write() source image is null");
        return false;
    }

    const QImage srcImage = preConvSrcImage.convertToFormat(QImage::Format_RGBA8888);
    const QSize size = srcImage.size();

    if (srcImage.isNull() || !size.isValid()) {
        qWarning("QHeifHandler::write() source image format conversion failed");
        return false;
    }

    // create dest image
    heif_image* destImagePtr = nullptr;
    auto error = heif_image_create(size.width(), size.height(),
                                   heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
                                   &destImagePtr);

    auto destImage = wrapPointer(destImagePtr, heif_image_release);
    if (error.code || !destImage) {
        qWarning("QHeifHandler::write() dest image creation failed: %s", error.message);
        return false;
    }

    // add rgba plane
    auto channel = heif_channel_interleaved;
    error = heif_image_add_plane(destImage.get(), channel,
                                 size.width(), size.height(), 32);

    if (error.code) {
        qWarning("QHeifHandler::write() failed to add image plane: %s", error.message);
        return false;
    }

    // get dest data
    int destStride = 0;
    uint8_t* destData = heif_image_get_plane(destImage.get(), channel, &destStride);

    if (!destData) {
        qWarning("QHeifHandler::write() could not get libheif image plane");
        return false;
    }

    if (destStride <= 0) {
        qWarning("QHeifHandler::write() invalid destination stride: %d", destStride);
        return false;
    }

    // get source data
    const uint8_t* srcData = srcImage.constBits();
    const int srcStride = srcImage.bytesPerLine();

    if (!srcData) {
        qWarning("QHeifHandler::write() source image data is null");
        return false;
    }

    if (srcStride <= 0) {
        qWarning("QHeifHandler::write() invalid source image stride: %d", srcStride);
        return false;
    } else if (srcStride > destStride) {
        qWarning("QHeifHandler::write() source line larger than destination");
        return false;
    }

    // copy rgba data
    for (int y = 0; y < size.height(); ++y) {
        auto* srcBegin = srcData + y * srcStride;
        auto* srcEnd = srcBegin + srcStride;
        std::copy(srcBegin, srcEnd, destData + y * destStride);
    }

    // get encoder
    heif_encoder* encoderPtr = nullptr;
    error = heif_context_get_encoder_for_format(nullptr, heif_compression_HEVC,
                                                &encoderPtr);

    auto encoder = wrapPointer(encoderPtr, heif_encoder_release);
    if (error.code || !encoder) {
        qWarning("QHeifHandler::write() failed to get encoder: %s", error.message);
        return false;
    }

    error = heif_encoder_set_lossy_quality(encoder.get(), _quality);
    if (error.code) {
        qWarning("QHeifHandler::write() failed to set quality: %s", error.message);
        return false;
    }

    // encode image
    auto context = wrapPointer(heif_context_alloc(), heif_context_free);
    if (!context) {
        qWarning("QHeifHandler::write() failed to alloc context");
        return false;
    }

    heif_image_handle* handlePtr = nullptr;
    error = heif_context_encode_image(context.get(), destImage.get(), encoder.get(),
                                      nullptr, &handlePtr);

    auto handle = wrapPointer(handlePtr, heif_image_handle_release);
    if (error.code || !handle) {
        qWarning("QHeifHandler::write() failed to encode image: %s", error.message);
        return false;
    }

    // write image
    heif_writer writer{1, handleWrite};
    error = heif_context_write(context.get(), &writer, device());
    if (error.code) {
        qWarning("QHeifHandler::write() failed to write image: %s", error.message);
        return false;
    }

    return true;
}

QVariant QHeifHandler::option(ImageOption opt) const
{
    Q_UNUSED(opt);
    switch (opt) {
    case Size:
        const_cast<QHeifHandler*>(this)->loadContext();
        if (!_readState || !_readState->imageHandle) {
            qWarning("QHeifHandler::read() failed to create context");
            return QSize();
        }
        return QSize(heif_image_handle_get_width(_readState->imageHandle.get()),
                     heif_image_handle_get_height(_readState->imageHandle.get()));

    default:
        return {};
    }
}

void QHeifHandler::setOption(ImageOption opt, const QVariant& value)
{
    switch (opt) {
    case Quality: {
        bool ok = false;
        int q = value.toInt(&ok);

        if (ok && q >= 0 && q <= 100) {
            _quality = q;
        }

        return;
    }

    default:
        return;
    }
}

bool QHeifHandler::supportsOption(ImageOption opt) const
{
    return opt == Quality
            || opt == Size;
}
