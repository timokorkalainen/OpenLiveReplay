#include "controlprotocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QJsonValue>
#include <cmath>
#include <QtGlobal>

namespace {

bool hasInteger(const QJsonObject& args, const QString& key) {
    const QJsonValue value = args.value(key);
    if (!value.isDouble()) return false;

    const double number = value.toDouble();
    if (!std::isfinite(number)) return false;
    if (std::floor(number) != number) return false;

    constexpr qint64 kSafeIntMin = -9007199254740991LL;
    constexpr qint64 kSafeIntMax = 9007199254740991LL;
    return number >= static_cast<double>(kSafeIntMin) && number <= static_cast<double>(kSafeIntMax);
}

bool hasNumber(const QJsonObject& args, const QString& key) {
    return args.value(key).isDouble();
}

bool hasBool(const QJsonObject& args, const QString& key) {
    return args.value(key).isBool();
}

bool hasString(const QJsonObject& args, const QString& key) {
    return args.value(key).isString();
}

ControlProtocol::CommandValidation valid(QJsonObject args = {}) {
    ControlProtocol::CommandValidation validation;
    validation.ok = true;
    validation.normalizedArgs = args;
    return validation;
}

ControlProtocol::CommandValidation invalid(const QString& message) {
    ControlProtocol::CommandValidation validation;
    validation.ok = false;
    validation.code = QStringLiteral("invalid_args");
    validation.message = message;
    return validation;
}

} // namespace

ControlProtocol::ParseResult ControlProtocol::parseTextMessage(const QByteArray& payload) {
    ParseResult result;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.code = QStringLiteral("bad_json");
        result.messageText = QStringLiteral("Invalid JSON: %1").arg(parseError.errorString());
        return result;
    }
    if (!doc.isObject()) {
        result.code = QStringLiteral("bad_message");
        result.messageText = QStringLiteral("Message must be a JSON object");
        return result;
    }

    const QJsonObject obj = doc.object();
    result.id = obj.value(QStringLiteral("id")).toString();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("command")) {
        result.code = QStringLiteral("unsupported_message");
        result.messageText = QStringLiteral("Unsupported message type");
        return result;
    }

    const QString name = obj.value(QStringLiteral("name")).toString();
    if (name.trimmed().isEmpty()) {
        result.code = QStringLiteral("bad_message");
        result.messageText = QStringLiteral("Command message requires name");
        return result;
    }

    result.ok = true;
    result.message.type = type;
    result.message.id = result.id;
    result.message.name = name;
    const QJsonValue args = obj.value(QStringLiteral("args"));
    result.message.args = args.isObject() ? args.toObject() : QJsonObject{};
    return result;
}

QJsonObject ControlProtocol::ack(const QString& id) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("ack"));
    if (!id.isEmpty()) obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), true);
    return obj;
}

QJsonObject ControlProtocol::ackError(const QString& id, const QString& code,
                                      const QString& message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("ack"));
    if (!id.isEmpty()) obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), false);
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    return obj;
}

QJsonObject ControlProtocol::error(const QString& code, const QString& message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("error"));
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    return obj;
}

QByteArray ControlProtocol::compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

ControlProtocol::CommandValidation
ControlProtocol::validateCommand(const ControlCommandMessage& command) {
    const QJsonObject args = command.args;
    const QString name = command.name;

    if (name == QStringLiteral("transport.playPause") || name == QStringLiteral("transport.play") ||
        name == QStringLiteral("transport.pause") || name == QStringLiteral("transport.goLive") ||
        name == QStringLiteral("transport.cancelFollowLive") ||
        name == QStringLiteral("recording.start") || name == QStringLiteral("recording.stop") ||
        name == QStringLiteral("recording.toggle") ||
        name == QStringLiteral("view.showMultiview") || name == QStringLiteral("capture.current") ||
        name == QStringLiteral("sources.add") || name == QStringLiteral("settings.save") ||
        name == QStringLiteral("import.read") || name == QStringLiteral("import.applyPreview") ||
        name == QStringLiteral("midi.refreshPorts") ||
        name == QStringLiteral("streamDeck.resetDefaults")) {
        return valid(args);
    }
    if (name == QStringLiteral("transport.seek")) {
        return hasInteger(args, QStringLiteral("positionMs"))
                   ? valid(args)
                   : invalid(QStringLiteral("transport.seek requires integer args.positionMs"));
    }
    if (name == QStringLiteral("transport.setSpeed")) {
        if (!hasNumber(args, QStringLiteral("speed"))) {
            return invalid(QStringLiteral("transport.setSpeed requires number args.speed"));
        }
        if (args.contains(QStringLiteral("playing")) && !hasBool(args, QStringLiteral("playing"))) {
            return invalid(QStringLiteral("transport.setSpeed args.playing must be boolean"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("transport.holdSpeed")) {
        if (!hasBool(args, QStringLiteral("active"))) {
            return invalid(QStringLiteral("transport.holdSpeed requires boolean args.active"));
        }
        if (args.value(QStringLiteral("active")).toBool() &&
            !hasNumber(args, QStringLiteral("speed"))) {
            return invalid(
                QStringLiteral("transport.holdSpeed requires number args.speed when active"));
        }
        if (args.contains(QStringLiteral("speed")) && !hasNumber(args, QStringLiteral("speed"))) {
            return invalid(QStringLiteral("transport.holdSpeed args.speed must be number"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("transport.stepFrame")) {
        return hasInteger(args, QStringLiteral("frames"))
                   ? valid(args)
                   : invalid(QStringLiteral("transport.stepFrame requires integer args.frames"));
    }
    if (name == QStringLiteral("view.setPlaybackViewState")) {
        if (!hasBool(args, QStringLiteral("singleView"))) {
            return invalid(
                QStringLiteral("view.setPlaybackViewState requires boolean args.singleView"));
        }
        if (!hasInteger(args, QStringLiteral("selectedIndex"))) {
            return invalid(
                QStringLiteral("view.setPlaybackViewState requires integer args.selectedIndex"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("view.selectFeed") ||
        name == QStringLiteral("view.toggleSourceEnabled") ||
        name == QStringLiteral("sources.remove") || name == QStringLiteral("midi.setPortIndex")) {
        return hasInteger(args, QStringLiteral("index"))
                   ? valid(args)
                   : invalid(name + QStringLiteral(" requires integer args.index"));
    }
    if (name == QStringLiteral("capture.snapshot")) {
        if (!hasBool(args, QStringLiteral("singleView"))) {
            return invalid(QStringLiteral("capture.snapshot requires boolean args.singleView"));
        }
        if (!hasInteger(args, QStringLiteral("selectedIndex"))) {
            return invalid(QStringLiteral("capture.snapshot requires integer args.selectedIndex"));
        }
        if (!hasInteger(args, QStringLiteral("playheadMs"))) {
            return invalid(QStringLiteral("capture.snapshot requires integer args.playheadMs"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("sources.updateUrl")) {
        if (!hasInteger(args, QStringLiteral("index"))) {
            return invalid(QStringLiteral("sources.updateUrl requires integer args.index"));
        }
        if (!hasString(args, QStringLiteral("url"))) {
            return invalid(QStringLiteral("sources.updateUrl requires string args.url"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("sources.updateName")) {
        if (!hasInteger(args, QStringLiteral("index"))) {
            return invalid(QStringLiteral("sources.updateName requires integer args.index"));
        }
        if (!hasString(args, QStringLiteral("name"))) {
            return invalid(QStringLiteral("sources.updateName requires string args.name"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("sources.updateId")) {
        if (!hasInteger(args, QStringLiteral("index"))) {
            return invalid(QStringLiteral("sources.updateId requires integer args.index"));
        }
        if (!hasString(args, QStringLiteral("id"))) {
            return invalid(QStringLiteral("sources.updateId requires string args.id"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("sources.setTrimOffset")) {
        if (!hasInteger(args, QStringLiteral("index"))) {
            return invalid(QStringLiteral("sources.setTrimOffset requires integer args.index"));
        }
        if (!hasInteger(args, QStringLiteral("ms"))) {
            return invalid(QStringLiteral("sources.setTrimOffset requires integer args.ms"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("sources.setMetadata")) {
        if (!hasInteger(args, QStringLiteral("index"))) {
            return invalid(QStringLiteral("sources.setMetadata requires integer args.index"));
        }
        if (!args.value(QStringLiteral("items")).isArray()) {
            return invalid(QStringLiteral("sources.setMetadata requires array args.items"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("settings.setProject")) {
        if (args.contains(QStringLiteral("fileName")) &&
            !hasString(args, QStringLiteral("fileName"))) {
            return invalid(QStringLiteral("settings.setProject args.fileName must be string"));
        }
        if (args.contains(QStringLiteral("saveLocation")) &&
            !hasString(args, QStringLiteral("saveLocation"))) {
            return invalid(QStringLiteral("settings.setProject args.saveLocation must be string"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("settings.setRecordingFormat")) {
        if (args.contains(QStringLiteral("width")) && !hasInteger(args, QStringLiteral("width"))) {
            return invalid(
                QStringLiteral("settings.setRecordingFormat args.width must be integer"));
        }
        if (args.contains(QStringLiteral("height")) &&
            !hasInteger(args, QStringLiteral("height"))) {
            return invalid(
                QStringLiteral("settings.setRecordingFormat args.height must be integer"));
        }
        if (args.contains(QStringLiteral("fps")) && !hasInteger(args, QStringLiteral("fps"))) {
            return invalid(QStringLiteral("settings.setRecordingFormat args.fps must be integer"));
        }
        return valid(args);
    }
    if (name == QStringLiteral("settings.setAudioOutputLatency")) {
        return hasInteger(args, QStringLiteral("ms"))
                   ? valid(args)
                   : invalid(
                         QStringLiteral("settings.setAudioOutputLatency requires integer args.ms"));
    }
    if (name == QStringLiteral("settings.setTimeOfDayMode")) {
        return hasBool(args, QStringLiteral("enabled"))
                   ? valid(args)
                   : invalid(
                         QStringLiteral("settings.setTimeOfDayMode requires boolean args.enabled"));
    }
    if (name == QStringLiteral("settings.setMetadataFields")) {
        return args.value(QStringLiteral("fields")).isArray()
                   ? valid(args)
                   : invalid(
                         QStringLiteral("settings.setMetadataFields requires array args.fields"));
    }
    if (name == QStringLiteral("import.setUrl")) {
        return hasString(args, QStringLiteral("url"))
                   ? valid(args)
                   : invalid(QStringLiteral("import.setUrl requires string args.url"));
    }
    if (name == QStringLiteral("midi.beginLearn") ||
        name == QStringLiteral("midi.beginLearnJogForward") ||
        name == QStringLiteral("midi.beginLearnJogBackward") ||
        name == QStringLiteral("midi.clearBinding") ||
        name == QStringLiteral("streamDeck.beginLearn") ||
        name == QStringLiteral("streamDeck.clearBinding")) {
        return hasInteger(args, QStringLiteral("action"))
                   ? valid(args)
                   : invalid(name + QStringLiteral(" requires integer args.action"));
    }
    if (name == QStringLiteral("action.dispatch")) {
        if (!hasInteger(args, QStringLiteral("actionId"))) {
            return invalid(QStringLiteral("action.dispatch requires integer args.actionId"));
        }
        if (args.value(QStringLiteral("actionId")).toInt() == 10) {
            return invalid(QStringLiteral(
                "action.dispatch actionId 10 requires action.shuttle with integer args.delta"));
        }
        QJsonObject normalized = args;
        if (!normalized.contains(QStringLiteral("pressed"))) {
            normalized.insert(QStringLiteral("pressed"), true);
        } else if (!hasBool(normalized, QStringLiteral("pressed"))) {
            return invalid(QStringLiteral("action.dispatch args.pressed must be boolean"));
        }
        return valid(normalized);
    }
    if (name == QStringLiteral("action.jog") || name == QStringLiteral("action.shuttle")) {
        return hasInteger(args, QStringLiteral("delta"))
                   ? valid(args)
                   : invalid(name + QStringLiteral(" requires integer args.delta"));
    }

    ControlProtocol::CommandValidation validation;
    validation.ok = false;
    validation.code = QStringLiteral("unknown_command");
    validation.message = QStringLiteral("Unknown command: %1").arg(name);
    return validation;
}
