#include "score/Serialize.h"

#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"

namespace fiddler::score {

QJsonArray toJson(const BarlineModel& m) {
    QJsonArray out;
    for (const auto ms : m.barlines()) {
        QJsonObject entry;
        entry["sourceMs"] = static_cast<qint64>(ms);
        out.append(entry);
    }
    return out;
}

QJsonArray toJson(const MarkerModel& m) {
    QJsonArray out;
    for (const auto& marker : m.markers()) {
        QJsonObject entry;
        entry["id"]       = static_cast<qint64>(marker.id);
        entry["sourceMs"] = static_cast<qint64>(marker.sourceMs);
        entry["name"]     = marker.name;
        out.append(entry);
    }
    return out;
}

QJsonArray toJson(const LoopModel& m) {
    QJsonArray out;
    for (const auto& loop : m.loops()) {
        QJsonObject entry;
        entry["id"]      = static_cast<qint64>(loop.id);
        entry["startMs"] = static_cast<qint64>(loop.startMs);
        entry["endMs"]   = static_cast<qint64>(loop.endMs);
        entry["name"]    = loop.name;
        out.append(entry);
    }
    return out;
}

QJsonArray toJson(const NoteModel& m) {
    QJsonArray out;
    for (const auto& note : m.notes()) {
        QJsonObject entry;
        entry["id"]      = static_cast<qint64>(note.id);
        entry["startMs"] = static_cast<qint64>(note.startMs);
        entry["endMs"]   = static_cast<qint64>(note.endMs);
        entry["midi"]    = note.midi;
        entry["name"]    = note.name;
        out.append(entry);
    }
    return out;
}

QJsonObject toJson(const TimeSignature& ts) {
    QJsonObject out;
    out["numerator"]   = ts.numerator;
    out["denominator"] = ts.denominator;
    out["tuneType"]    = ts.tuneType;
    return out;
}

bool loadFrom(BarlineModel& m, const QJsonArray& arr) {
    m.clear();
    for (const auto& v : arr) {
        if (!v.isObject()) { m.clear(); return false; }
        const auto obj = v.toObject();
        const auto msVal = obj.value("sourceMs");
        if (!msVal.isDouble()) { m.clear(); return false; }
        m.add(static_cast<std::int64_t>(msVal.toDouble()));
    }
    return true;
}

bool loadFrom(MarkerModel& m, const QJsonArray& arr) {
    m.clear();
    for (const auto& v : arr) {
        if (!v.isObject()) { m.clear(); return false; }
        const auto obj = v.toObject();
        const auto idVal = obj.value("id");
        const auto msVal = obj.value("sourceMs");
        if (!idVal.isDouble() || !msVal.isDouble()) {
            m.clear();
            return false;
        }
        const auto id = static_cast<std::int64_t>(idVal.toDouble());
        const auto ms = static_cast<std::int64_t>(msVal.toDouble());
        const QString name = obj.value("name").toString();
        if (!m.addWithId(id, ms, name)) { m.clear(); return false; }
    }
    return true;
}

bool loadFrom(LoopModel& m, const QJsonArray& arr) {
    m.clear();
    for (const auto& v : arr) {
        if (!v.isObject()) { m.clear(); return false; }
        const auto obj = v.toObject();
        const auto idVal    = obj.value("id");
        const auto startVal = obj.value("startMs");
        const auto endVal   = obj.value("endMs");
        if (!idVal.isDouble() || !startVal.isDouble() || !endVal.isDouble()) {
            m.clear();
            return false;
        }
        const auto id      = static_cast<std::int64_t>(idVal.toDouble());
        const auto startMs = static_cast<std::int64_t>(startVal.toDouble());
        const auto endMs   = static_cast<std::int64_t>(endVal.toDouble());
        const QString name = obj.value("name").toString();
        if (!m.addWithId(id, startMs, endMs, name)) {
            m.clear();
            return false;
        }
    }
    return true;
}

bool loadFrom(NoteModel& m, const QJsonArray& arr) {
    m.clear();
    for (const auto& v : arr) {
        if (!v.isObject()) { m.clear(); return false; }
        const auto obj = v.toObject();
        const auto idVal    = obj.value("id");
        const auto startVal = obj.value("startMs");
        const auto endVal   = obj.value("endMs");
        const auto midiVal  = obj.value("midi");
        if (!idVal.isDouble()    || !startVal.isDouble()
         || !endVal.isDouble()   || !midiVal.isDouble()) {
            m.clear();
            return false;
        }
        const auto id      = static_cast<std::int64_t>(idVal.toDouble());
        const auto startMs = static_cast<std::int64_t>(startVal.toDouble());
        const auto endMs   = static_cast<std::int64_t>(endVal.toDouble());
        const int  midi    = midiVal.toInt();
        const QString name = obj.value("name").toString();
        if (!m.addWithId(id, startMs, endMs, midi, name)) {
            m.clear();
            return false;
        }
    }
    return true;
}

TimeSignature timeSignatureFromJson(const QJsonObject& obj) {
    TimeSignature ts;
    if (obj.value("numerator").isDouble()) {
        ts.numerator = obj.value("numerator").toInt();
    }
    if (obj.value("denominator").isDouble()) {
        ts.denominator = obj.value("denominator").toInt();
    }
    ts.tuneType = obj.value("tuneType").toString();
    return ts;
}

} // namespace fiddler::score
