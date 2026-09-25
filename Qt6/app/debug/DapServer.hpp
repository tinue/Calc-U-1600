#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

// Debug Adapter Protocol transport: a TCP server on 127.0.0.1 only, one
// client at a time, messages framed by a "Content-Length: n" header
// (the DAP base protocol). A second connection is told it's busy and
// closed. Emits each decoded message; send() frames and writes one.
class DapServer : public QObject {
    Q_OBJECT
public:
    explicit DapServer(QObject* parent = nullptr);
    ~DapServer() override;

    /// (Re)binds to `port`; false with `error` set if that fails (port in use).
    bool listen(quint16 port, QString* error);
    void close();
    bool isListening() const;
    quint16 port() const;
    bool hasClient() const { return m_client != nullptr; }

    void send(const QJsonObject& message);
    /// Drops the client (after the last message has been flushed).
    void disconnectClient();

signals:
    void clientConnected();
    void clientDisconnected();
    void messageReceived(const QJsonObject& message);

private:
    void onNewConnection();
    void onReadyRead();

    QTcpServer* m_server = nullptr;
    QTcpSocket* m_client = nullptr;
    QByteArray m_buffer;
};
