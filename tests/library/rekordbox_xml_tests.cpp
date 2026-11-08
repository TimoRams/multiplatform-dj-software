#include "library/RekordboxXml.h"

#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace brockdj::library;

namespace {

void require(bool value, const QString& message)
{
    if (!value) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        std::exit(1);
    }
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<DJ_PLAYLISTS Version="1.0.0">
  <PRODUCT Name="rekordbox" Version="6.8.0" Company="AlphaTheta"/>
  <COLLECTION Entries="2">
    <TRACK TrackID="11" Name="Strobe" Artist="deadmau5" Album="For Lack" Genre="Progressive"
           Kind="MP3 File" TotalTime="12" DateAdded="2024-03-05" BitRate="320"
           AverageBpm="128.00" Rating="204" PlayCount="7" Comments="peak" Colour="0xFF007F"
           Tonality="Am" Location="file://localhost/C:/Music/My%20Songs/strobe.mp3">
      <TEMPO Inizio="0.500" Bpm="120.00" Metro="4/4" Battito="1"/>
      <TEMPO Inizio="4.500" Bpm="60.00" Metro="4/4" Battito="1"/>
      <POSITION_MARK Name="Drop" Type="0" Start="1.000" Num="0" Red="40" Green="226" Blue="20"/>
      <POSITION_MARK Name="" Type="0" Start="0.500" Num="-1"/>
      <POSITION_MARK Name="Loop" Type="4" Start="2.000" End="4.000" Num="2"/>
      <POSITION_MARK Name="Fade" Type="1" Start="3.000" Num="-1"/>
    </TRACK>
    <TRACK TrackID="12" Name="Other &amp; Co" Artist="" TotalTime="0" Rating="0"
           Tonality="8A" Location="file://localhost/Users/dj/M%C3%BCsik/other.flac"/>
  </COLLECTION>
  <PLAYLISTS>
    <NODE Type="0" Name="ROOT" Count="2">
      <NODE Name="Sets" Type="0" Count="1">
        <NODE Name="Friday" Type="1" KeyType="0" Entries="2">
          <TRACK Key="12"/>
          <TRACK Key="11"/>
        </NODE>
      </NODE>
      <NODE Name="ByPath" Type="1" KeyType="1" Entries="1">
        <TRACK Key="file://localhost/Users/dj/M%C3%BCsik/other.flac"/>
      </NODE>
    </NODE>
  </PLAYLISTS>
</DJ_PLAYLISTS>
)";

} // namespace

int main()
{
    require(rekordboxLocationToPath("file://localhost/C:/A%20B/x.mp3") == "C:/A B/x.mp3", "windows location");
    require(rekordboxLocationToPath("file://localhost/Users/x/y.mp3") == "/Users/x/y.mp3", "mac location");
    require(rekordboxLocationToPath("file:///home/a/b.wav") == "/home/a/b.wav", "plain file url");

    require(musicalKeyToCamelot("Am") == "8A", "Am");
    require(musicalKeyToCamelot("F#m") == "11A", "F#m");
    require(musicalKeyToCamelot("Ebm") == "2A", "Ebm");
    require(musicalKeyToCamelot("C") == "8B", "C");
    require(musicalKeyToCamelot("Bb") == "6B", "Bb");
    require(musicalKeyToCamelot("08A") == "8A", "camelot passthrough");
    require(musicalKeyToCamelot("12b") == "12B", "camelot case");
    require(musicalKeyToCamelot("A minor") == "8A", "long minor");
    require(musicalKeyToCamelot("H").isEmpty(), "unknown key");

    RekordboxXmlLibrary lib;
    QString error;
    require(parseRekordboxXml(QByteArray(kXml), lib, &error), "parse: " + error);
    require(lib.tracks.size() == 2, "track count");

    const RekordboxXmlTrack& t = lib.tracks[0];
    require(t.key == "11" && t.path == "C:/Music/My Songs/strobe.mp3", "track 1 identity");
    require(t.title == "Strobe" && t.artist == "deadmau5" && t.album == "For Lack" && t.genre == "Progressive",
            "track 1 tags");
    require(t.durationSec == 12 && t.bitrateKbps == 320 && near(t.bpm, 128.0), "track 1 numbers");
    require(t.rating == 4 && t.playCount == 7 && t.comment == "peak", "rating/plays/comment");
    require(t.color == "#ff007f", "colour: " + t.color);
    require(t.musicalKey == "8A", "tonality");
    require(t.dateAdded == 1709596800, "date added");
    require(t.tempos.size() == 2 && near(t.tempos[1].positionSec, 4.5) && near(t.tempos[1].bpm, 60.0), "tempos");
    require(t.marks.size() == 3, "fade marker skipped");
    require(t.marks[0].hotCueIndex == 0 && !t.marks[0].loop && t.marks[0].name == "Drop"
                && t.marks[0].color == "#28e214", "hot cue");
    require(t.marks[1].hotCueIndex == -1 && near(t.marks[1].startSec, 0.5), "memory cue");
    require(t.marks[2].loop && t.marks[2].hotCueIndex == 2 && near(t.marks[2].endSec, 4.0), "loop");

    const RekordboxXmlTrack& o = lib.tracks[1];
    require(o.title == "Other & Co" && o.path == QString::fromUtf8("/Users/dj/Müsik/other.flac"), "track 2 path");
    require(o.rating == 0 && o.musicalKey == "8A" && o.color.isEmpty() && o.dateAdded == 0, "track 2 defaults");

    require(lib.playlists.size() == 2, "root children");
    require(lib.playlists[0].folder && lib.playlists[0].name == "Sets" && lib.playlists[0].children.size() == 1, "folder");
    const RekordboxXmlNode& friday = lib.playlists[0].children[0];
    require(!friday.folder && friday.name == "Friday" && friday.trackKeys == QStringList({"12", "11"}), "playlist keys");
    require(lib.playlists[1].trackKeys == QStringList({QString::fromUtf8("/Users/dj/Müsik/other.flac")}),
            "path keyed playlist");

    // 120 BPM from 0.5 s: 0.5,1.0,...,4.0 (8 beats), then 60 BPM from 4.5 up to 12 s.
    const QList<RekordboxBeat> beats = expandRekordboxBeats(t.tempos, t.durationSec);
    require(beats.size() == 8 + 8, "beat count: " + QString::number(beats.size()));
    require(near(beats[0].positionSec, 0.5) && beats[0].beatInBar == 1, "first beat");
    require(near(beats[7].positionSec, 4.0) && beats[7].beatInBar == 4, "last 120 beat");
    require(near(beats[8].positionSec, 4.5) && beats[8].beatInBar == 1 && near(beats[8].bpm, 60.0), "tempo change");
    require(near(beats.last().positionSec, 11.5), "last beat");
    require(expandRekordboxBeats({}, 100).isEmpty(), "no tempo");
    require(expandRekordboxBeats({{0.0, 0.0, 1}}, 100).isEmpty(), "zero bpm ignored");

    RekordboxXmlLibrary bad;
    require(!parseRekordboxXml("<NML/>", bad, &error), "non rekordbox rejected");
    require(!parseRekordboxXml("<DJ_PLAYLISTS><COLLECTION>", bad, &error), "truncated rejected");

    QTemporaryDir dir;
    QFile file(dir.filePath("rb.xml"));
    require(file.open(QIODevice::WriteOnly) && file.write(kXml) > 0, "write fixture");
    file.close();
    RekordboxXmlLibrary fromFile;
    require(readRekordboxXml(file.fileName(), fromFile, &error) && fromFile.tracks.size() == 2, "read file");
    require(!readRekordboxXml(dir.filePath("missing.xml"), fromFile, &error), "missing file");

    std::cout << "rekordbox xml tests passed\n";
    return 0;
}
