#pragma once

#include <QString>

#include <functional>

namespace papertrail::core {
class Database;
}

namespace papertrail::import {

struct RecognitionOptions {
    QString workerPath;
    QString cacheDirectory; // Recognised words are kept in a folder inside this one: see recognisedFolderFor.
    QString tessdata;       // The folder with the languages' models.
    QString languages;      // Tesseract's codes joined by "+", such as "eng+fra".
    int timeoutMs = 600000; // For one page, which is what a bad page can hold up.
    // Checked while waiting for the worker, so that stopping doesn't have to wait for a large file.
    std::function<bool()> cancelled;
};

struct RecognitionReport {
    int documents = 0; // Finished in this run.
    int pages = 0;     // Recognised in this run.
    bool stopped = false;
    QString problem; // Such as a language that's missing, which stops the run.
};

// Recognises the words on the pages of scanned documents, those whose files have no text of their own, a page at a
// time through the worker, at the worker's low priority. Where it's got to is kept beside the full-text index, so
// stopping is safe and it carries on where it left off. A document with pages recognised is read into the index
// again, with its words. The database connection needs the index attached.
//
// A document whose own text is poor, such as an old paper scanned with a publisher's text layer, can be asked for in
// full with want(): those come first, every page of them is recognised, and their recognised words are used in place
// of their own from then on.
class TextRecognition {
public:
    TextRecognition(core::Database &db, const RecognitionOptions &options);

    // Called after each page.
    using Progress = std::function<void(qint64 documentsLeft, int page, int pageCount, const QString &fileName)>;
    RecognitionReport run(const Progress &progress = {});

    // How many documents are waiting to be recognised with these languages.
    qint64 waitingCount();

    // Asks for every page of the file with these contents to be recognised, starting again if it's been done.
    static void want(core::Database &db, const QString &sha256);
    static bool isWanted(core::Database &db, const QString &sha256);

private:
    core::Database &m_db;
    RecognitionOptions m_options;
};

} // namespace papertrail::import
