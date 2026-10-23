#include "hkjc_provider.hpp"
#include <QAuthenticator>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QTimer>

namespace marksix::statistics {
std::string hkjcHttps(const HkjcRequest& input, std::stop_token stop) {
    if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled);
    validateHkjcRequest(input);
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(QString::fromStdString("https://" + input.host + input.path)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(5000);
    request.setRawHeader("User-Agent", "MarkSixEmulator/1.0 local-history");
    request.setRawHeader("Accept", input.body.empty() ? "text/html, application/javascript" : "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = input.body.empty() ? manager.get(request) : manager.post(request, QByteArray::fromStdString(input.body));
    reply->setReadBufferSize(32768);
    QEventLoop loop;
    QTimer deadline, cancellation;
    deadline.setSingleShot(true);
    std::optional<UpdateFailure> failure;
    const auto fail = [&](UpdateFailure category) { if (!failure) failure = category; reply->abort(); };
    std::string output;
    const auto read = [&] {
        const auto bytes = reply->readAll();
        if (size_t(bytes.size()) > input.maximumBytes - output.size()) { fail(UpdateFailure::Schema); return; }
        output.append(bytes.constData(), size_t(bytes.size()));
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, read);
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!status.isValid()) return;
        try { checkHkjcHttpStatus(status.toUInt()); }
        catch (const ProviderFailure& error) { fail(error.category); }
    });
    QObject::connect(reply, &QNetworkReply::sslErrors, &loop, [&](const QList<QSslError>&) { fail(UpdateFailure::Tls); });
    QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired, &loop,
                     [&](QNetworkReply*, QAuthenticator*) { fail(UpdateFailure::Denied); });
    QObject::connect(&manager, &QNetworkAccessManager::proxyAuthenticationRequired, &loop,
                     [&](const QNetworkProxy&, QAuthenticator*) { fail(UpdateFailure::Denied); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { fail(UpdateFailure::Connection); });
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] { if (stop.stop_requested()) fail(UpdateFailure::Cancelled); });
    deadline.start(25000); cancellation.start(50);
    if (!reply->isFinished()) loop.exec();
    if (!failure) read();
    if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled);
    if (failure) throw ProviderFailure(*failure);
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) checkHkjcHttpStatus(status.toUInt());
    if (reply->error() != QNetworkReply::NoError || !status.isValid()) throw ProviderFailure(UpdateFailure::Connection);
    return output;
}
}
