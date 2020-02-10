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

#include <qimageiohandler.h>
#include <qstringlist.h>

#ifndef QT_NO_IMAGEFORMATPLUGIN

#include "qheifhandler_p.h"

#include <qiodevice.h>
#include <qbytearray.h>

QT_BEGIN_NAMESPACE

class QHeifPlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "heif.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override;
    QImageIOHandler *create(QIODevice *device, const QByteArray &format = QByteArray()) const override;
};

QHeifPlugin::Capabilities QHeifPlugin::capabilities(QIODevice *device, const QByteArray &format) const
{
    const bool formatOK = (format == "heic" || format == "heics"
                           || format == "heif" || format == "heifs");

    if (!formatOK && !format.isEmpty()) {
        return {};
    }

    if (!device) {
        if (formatOK) {
            return CanRead | CanWrite;
        } else {
            return {};
        }
    }

    Capabilities caps{};
    if (device->isReadable() && QHeifHandler::canReadFrom(*device)) {
        caps |= CanRead;
    }

    if (device->isWritable()) {
        caps |= CanWrite;
    }

    return caps;
}

QImageIOHandler *QHeifPlugin::create(QIODevice *device, const QByteArray &format) const
{
    QImageIOHandler *handler = new QHeifHandler;
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}

QT_END_NAMESPACE

#include "main.moc"

#endif // !QT_NO_IMAGEFORMATPLUGIN