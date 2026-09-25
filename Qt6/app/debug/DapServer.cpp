#include "DapServer.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>

DapServer::DapServer(QObject* parent) : QObject(parent) {
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &DapServer::onNewConnection);
}

DapServer::~DapServer() { close(); }

bool DapServer::listen(quint16 port, QString* error) {
    close();
    if (m_server->listen(QHostAddress::LocalHost, port)) return true;
    if (error) *error = m_server->errorString();
    return false;
}

void DapServer::close() {
    disconnectClient();
    m_server->close();
}

bool DapServer::isListening() const { return m_server->isListening(); }
quint16 DapServer::port() const { return m_server->serverPort(); }

void DapServer::onNewConnection() {
    while (QTcpSocket* socket = m_server->nextPendingConnection()) {
        if (m_client) {
            // One debug session at a time: tell the newcomer why it's refused.
            QJsonObject busy{{QStringLiteral("seq"), 0},
                             {QStringLiteral("type"), QStringLiteral("event")},
                             {QStringLiteral("event"), QStringLiteral("output")},
                             {QStringLiteral("body"),
                              QJsonObject{{QStringLiteral("category"), QStringLiteral("important")},
                                          {QStringLiteral("output"),
                                           QStringLiteral("Calc-U-1600 already has a debugger attached.\n")}}}};
            const QByteArray json = QJsonDocument(busy).toJson(QJsonDocument::Compact);
            socket->write("Content-Length: " + QByteArray::number(json.size()) + "\r\n\r\n" + json);
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        m_client = socket;
        m_buffer.clear();
        connect(socket, &QTcpSocket::readyRead, this, &DapServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            socket->deleteLater();
            if (m_client != socket) return;
            m_client = nullptr;
            m_buffer.clear();
            emit clientDisconnected();
        });
        emit clientConnected();
    }
}

void DapServer::onReadyRead() {
    if (!m_client) return;
    m_buffer += m_client->readAll();
    for (;;) {
        const int headerEnd = m_buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        int length = -1;
        for (const QByteArray& line : m_buffer.left(headerEnd).split('\n')) {
            const QByteArray l = line.trimmed();
            if (l.toLower().startsWith("content-length:")) length = l.mid(15).trimmed().toInt();
        }
        if (length < 0) { // not DAP: drop the garbage header
            m_buffer.remove(0, headerEnd + 4);
            continue;
        }
        if (m_buffer.size() < headerEnd + 4 + length) return;
        const QByteArray body = m_buffer.mid(headerEnd + 4, length);
        m_buffer.remove(0, headerEnd + 4 + length);
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) emit messageReceived(doc.object());
        if (!m_client) return; // the handler disconnected
    }
}

void DapServer::send(const QJsonObject& message) {
    if (!m_client) return;
    const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
    m_client->write("Content-Length: " + QByteArray::number(json.size()) + "\r\n\r\n" + json);
}

void DapServer::disconnectClient() {
    if (!m_client) return;
    QTcpSocket* socket = m_client;
    m_client = nullptr;
    m_buffer.clear();
    socket->flush();
    socket->disconnectFromHost();
    emit clientDisconnected();
}
