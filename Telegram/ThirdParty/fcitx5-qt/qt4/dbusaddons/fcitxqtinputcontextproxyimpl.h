/*
 * This file was generated by qdbusxml2cpp version 0.7
 * Command line was: qdbusxml2cpp -N -p fcitxqtinputcontextproxyimpl -c
 * FcitxQtInputContextProxyImpl interfaces/org.fcitx.Fcitx.InputContext1.xml -i
 * fcitxqtdbustypes.h -i fcitx5qt4dbusaddons_export.h
 *
 * qdbusxml2cpp is Copyright (C) 2015 The Qt Company Ltd.
 *
 * This is an auto-generated file.
 * Do not edit! All changes made to it will be lost.
 */

#ifndef FCITXQTINPUTCONTEXTPROXYIMPL_H_1637715099
#define FCITXQTINPUTCONTEXTPROXYIMPL_H_1637715099

#include "fcitx5qt4dbusaddons_export.h"
#include "fcitxqtdbustypes.h"
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtDBus/QtDBus>

namespace fcitx {

/*
 * Proxy class for interface org.fcitx.Fcitx.InputContext1
 */
class FcitxQtInputContextProxyImpl : public QDBusAbstractInterface {
    Q_OBJECT
public:
    static inline const char *staticInterfaceName() {
        return "org.fcitx.Fcitx.InputContext1";
    }

public:
    FcitxQtInputContextProxyImpl(const QString &service, const QString &path,
                                 const QDBusConnection &connection,
                                 QObject *parent = 0);

    ~FcitxQtInputContextProxyImpl();

public Q_SLOTS: // METHODS
    inline QDBusPendingReply<> DestroyIC() {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QLatin1String("DestroyIC"),
                                         argumentList);
    }

    inline QDBusPendingReply<> FocusIn() {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QLatin1String("FocusIn"),
                                         argumentList);
    }

    inline QDBusPendingReply<> FocusOut() {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QLatin1String("FocusOut"),
                                         argumentList);
    }

    inline QDBusPendingReply<> InvokeAction(unsigned int action, int cursor) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(action)
                     << QVariant::fromValue(cursor);
        return asyncCallWithArgumentList(QLatin1String("InvokeAction"),
                                         argumentList);
    }

    inline QDBusPendingReply<> NextPage() {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QLatin1String("NextPage"),
                                         argumentList);
    }

    inline QDBusPendingReply<> PrevPage() {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QLatin1String("PrevPage"),
                                         argumentList);
    }

    inline QDBusPendingReply<bool> ProcessKeyEvent(unsigned int keyval, unsigned int keycode,
                                                   unsigned int state, bool type,
                                                   unsigned int time) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(keyval)
                     << QVariant::fromValue(keycode)
                     << QVariant::fromValue(state) << QVariant::fromValue(type)
                     << QVariant::fromValue(time);
        return asyncCallWithArgumentList(QLatin1String("ProcessKeyEvent"),
                                         argumentList);
    }

    inline QDBusPendingReply<> Reset() {
        QList<QVariant> argumentList;
        return asyncCallWithArgumentList(QLatin1String("Reset"), argumentList);
    }

    inline QDBusPendingReply<> SelectCandidate(int index) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(index);
        return asyncCallWithArgumentList(QLatin1String("SelectCandidate"),
                                         argumentList);
    }

    inline QDBusPendingReply<> SetCapability(qulonglong caps) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(caps);
        return asyncCallWithArgumentList(QLatin1String("SetCapability"),
                                         argumentList);
    }

    inline QDBusPendingReply<> SetCursorRect(int x, int y, int w, int h) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(x) << QVariant::fromValue(y)
                     << QVariant::fromValue(w) << QVariant::fromValue(h);
        return asyncCallWithArgumentList(QLatin1String("SetCursorRect"),
                                         argumentList);
    }

    inline QDBusPendingReply<> SetCursorRectV2(int x, int y, int w, int h,
                                               double scale) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(x) << QVariant::fromValue(y)
                     << QVariant::fromValue(w) << QVariant::fromValue(h)
                     << QVariant::fromValue(scale);
        return asyncCallWithArgumentList(QLatin1String("SetCursorRectV2"),
                                         argumentList);
    }

    inline QDBusPendingReply<> SetSurroundingText(const QString &text,
                                                  unsigned int cursor, unsigned int anchor) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(text) << QVariant::fromValue(cursor)
                     << QVariant::fromValue(anchor);
        return asyncCallWithArgumentList(QLatin1String("SetSurroundingText"),
                                         argumentList);
    }

    inline QDBusPendingReply<> SetSurroundingTextPosition(unsigned int cursor,
                                                          unsigned int anchor) {
        QList<QVariant> argumentList;
        argumentList << QVariant::fromValue(cursor)
                     << QVariant::fromValue(anchor);
        return asyncCallWithArgumentList(
            QLatin1String("SetSurroundingTextPosition"), argumentList);
    }

Q_SIGNALS: // SIGNALS
    void CommitString(const QString &str);
    void CurrentIM(const QString &name, const QString &uniqueName,
                   const QString &langCode);
    void DeleteSurroundingText(int offset, unsigned int nchar);
    void ForwardKey(unsigned int keyval, unsigned int state, bool type);
    void UpdateClientSideUI(FcitxQtFormattedPreeditList preedit, int cursorpos,
                            FcitxQtFormattedPreeditList auxUp,
                            FcitxQtFormattedPreeditList auxDown,
                            FcitxQtFormattedPreeditList candidates,
                            int candidateIndex, int layoutHint, bool hasPrev,
                            bool hasNext);
    void UpdateFormattedPreedit(FcitxQtFormattedPreeditList str, int cursorpos);
};
} // namespace fcitx

#endif