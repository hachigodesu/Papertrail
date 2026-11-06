#include "SyntheticLibrary.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStringList>

#include <cmath>

namespace papertrail::tools {

using namespace core;

namespace {

const QStringList kFamilies = {
    QStringLiteral("Smith"), QStringLiteral("Müller"), QStringLiteral("García"), QStringLiteral("Wang"),
    QStringLiteral("Tanaka"), QStringLiteral("Novák"), QStringLiteral("O'Brien"), QStringLiteral("Singh"),
    QStringLiteral("Kowalski"), QStringLiteral("Dubois"), QStringLiteral("Rossi"), QStringLiteral("Silva"),
    QStringLiteral("Kim"), QStringLiteral("Ivanov"), QStringLiteral("Hansen"), QStringLiteral("Nguyen"),
    QStringLiteral("Okafor"), QStringLiteral("Haddad"), QStringLiteral("van der Berg"), QStringLiteral("Papadopoulos"),
    QStringLiteral("Johansson"), QStringLiteral("Yilmaz"), QStringLiteral("Cohen"), QStringLiteral("Fernández"),
};

const QStringList kGivens = {
    QStringLiteral("Alice"), QStringLiteral("Bjorn"), QStringLiteral("Chen"), QStringLiteral("Dara"),
    QStringLiteral("Elif"), QStringLiteral("Farid"), QStringLiteral("Grace"), QStringLiteral("Hiro"),
    QStringLiteral("Ines"), QStringLiteral("Jonas"), QStringLiteral("Kavya"), QStringLiteral("Luis"),
    QStringLiteral("Mei"), QStringLiteral("Nadia"), QStringLiteral("Omar"), QStringLiteral("Priya"),
    QStringLiteral("Quinn"), QStringLiteral("Rosa"), QStringLiteral("Sven"), QStringLiteral("Tara"),
    QStringLiteral("Uma"), QStringLiteral("Viktor"), QStringLiteral("Wen"), QStringLiteral("Yara"),
};

const QStringList kWords = {
    QStringLiteral("analysis"), QStringLiteral("finite"), QStringLiteral("element"), QStringLiteral("nonlinear"),
    QStringLiteral("dynamics"), QStringLiteral("structural"), QStringLiteral("optimisation"), QStringLiteral("machine"),
    QStringLiteral("learning"), QStringLiteral("neural"), QStringLiteral("network"), QStringLiteral("quantum"),
    QStringLiteral("thermal"), QStringLiteral("fatigue"), QStringLiteral("corrosion"), QStringLiteral("composite"),
    QStringLiteral("materials"), QStringLiteral("distributed"), QStringLiteral("systems"), QStringLiteral("protein"),
    QStringLiteral("genomic"), QStringLiteral("clinical"), QStringLiteral("cohort"), QStringLiteral("statistical"),
    QStringLiteral("inference"), QStringLiteral("bayesian"), QStringLiteral("stochastic"), QStringLiteral("control"),
    QStringLiteral("robust"), QStringLiteral("adaptive"), QStringLiteral("model"), QStringLiteral("simulation"),
    QStringLiteral("experimental"), QStringLiteral("validation"), QStringLiteral("turbulent"), QStringLiteral("flow"),
    QStringLiteral("boundary"), QStringLiteral("layer"), QStringLiteral("graph"), QStringLiteral("algorithm"),
    QStringLiteral("complexity"), QStringLiteral("cryptographic"), QStringLiteral("protocol"), QStringLiteral("seismic"),
    QStringLiteral("reinforced"), QStringLiteral("concrete"), QStringLiteral("steel"), QStringLiteral("hydraulic"),
    QStringLiteral("climate"), QStringLiteral("ecosystem"), QStringLiteral("population"), QStringLiteral("policy"),
    QStringLiteral("linguistic"), QStringLiteral("semantic"), QStringLiteral("historical"), QStringLiteral("survey"),
    QStringLiteral("review"), QStringLiteral("framework"), QStringLiteral("method"), QStringLiteral("approach"),
};

const QStringList kTagRoots = {
    QStringLiteral("Engineering"), QStringLiteral("Computer Science"), QStringLiteral("Mathematics"),
    QStringLiteral("Physics"), QStringLiteral("Chemistry"), QStringLiteral("Biology"), QStringLiteral("Medicine"),
    QStringLiteral("Earth Sciences"), QStringLiteral("Economics"), QStringLiteral("Psychology"),
    QStringLiteral("History"), QStringLiteral("Linguistics"),
};

const QStringList kFormats = {
    QStringLiteral("pdf"), QStringLiteral("pdf"), QStringLiteral("pdf"), QStringLiteral("pdf"),
    QStringLiteral("pdf"), QStringLiteral("pdf"), QStringLiteral("pdf"), QStringLiteral("pdf"),
    QStringLiteral("epub"), QStringLiteral("djvu"),
};

const QStringList kLanguages = {
    QStringLiteral("en"), QStringLiteral("en"), QStringLiteral("en"), QStringLiteral("en"), QStringLiteral("en"),
    QStringLiteral("en"), QStringLiteral("en"), QStringLiteral("de"), QStringLiteral("fr"), QStringLiteral("zh"),
};

class Generator {
public:
    Generator(Database &db, const SyntheticOptions &options)
        : m_db(db), m_options(options), m_random(options.seed), m_publications(db), m_tags(db), m_files(db)
    {
    }

    SyntheticStats run()
    {
        Transaction transaction(m_db);
        m_db.exec("PRAGMA defer_foreign_keys = ON");
        buildPools();

        SyntheticStats stats;
        QList<qint64> volumes;
        for (int i = 0; i < m_options.publications; ++i) {
            Publication p = makePublication(i);

            // About one article in four belongs to a volume, as in a real journal collection.
            if (p.type == PublicationType::JournalArticle && !volumes.isEmpty() && chance(25)) {
                p.parentId = volumes.at(int(m_random.bounded(quint32(volumes.size()))));
                p.pageStart = between(1, 800);
                p.pageEnd = *p.pageStart + between(4, 30);
            }

            const qint64 id = m_publications.create(p);
            ++stats.publications;
            if (p.type == PublicationType::JournalVolume)
                volumes << id;

            const int tagCount = between(0, 5);
            for (int t = 0; t < tagCount; ++t)
                m_tags.assign(id, pick(m_tagIds));

            // Articles inside a volume use the volume's file. A few others have no file yet.
            if (!p.parentId && !chance(4)) {
                addFile(id, i);
                ++stats.files;
            }
        }
        transaction.commit();

        const auto scalar = [this](const char *sql) {
            auto select = m_db.prepare(sql);
            select->step();
            return select->int64At(0);
        };
        stats.people = scalar("SELECT count(*) FROM person");
        stats.tags = scalar("SELECT count(*) FROM tag");
        return stats;
    }

private:
    int between(int low, int high) { return m_random.bounded(low, high + 1); }
    bool chance(int percent) { return between(1, 100) <= percent; }

    template <typename T>
    const T &pick(const QList<T> &list)
    {
        return list.at(int(m_random.bounded(quint32(list.size()))));
    }

    // A few values get most of the use, as in real collections: squaring the random
    // position favours the start of the list.
    template <typename T>
    const T &pickSkewed(const QList<T> &list)
    {
        const double position = m_random.generateDouble();
        return list.at(int(position * position * list.size()));
    }

    QString words(int count)
    {
        QStringList chosen;
        for (int i = 0; i < count; ++i)
            chosen << pick(kWords);
        return chosen.join(u' ');
    }

    QString capitalised(const QString &text) { return text.left(1).toUpper() + text.mid(1); }

    void buildPools()
    {
        for (int i = 0; i < 400; ++i)
            m_journals << QStringLiteral("Journal of %1").arg(capitalised(words(2)));
        for (int i = 0; i < 60; ++i)
            m_publishers << QStringLiteral("%1 Press").arg(capitalised(pick(kWords)));
        for (int i = 0; i < 150; ++i)
            m_conferences << QStringLiteral("International Conference on %1").arg(capitalised(words(2)));
        for (int i = 0; i < 200; ++i)
            m_institutions << QStringLiteral("University of %1").arg(capitalised(pick(kWords)));
        for (int i = 0; i < 30000; ++i)
            m_people << PersonName{pick(kFamilies) + QString::number(i % 1000), pick(kGivens)};

        for (const QString &root : kTagRoots) {
            const qint64 rootId = m_tags.create(root);
            m_tagIds << rootId;
            for (int i = 0; i < 4; ++i) {
                const qint64 middle = m_tags.create(capitalised(words(2)) + QString::number(i), rootId);
                m_tagIds << middle;
                for (int j = 0; j < 4; ++j)
                    m_tagIds << m_tags.create(capitalised(words(2)) + QString::number(j), middle);
            }
        }

        m_rootIds << m_files.addRoot(QStringLiteral("Papers"), QStringLiteral("/synthetic/papers"));
        m_rootIds << m_files.addRoot(QStringLiteral("Books"), QStringLiteral("/synthetic/books"));
    }

    PublicationType pickType()
    {
        const int roll = between(1, 100);
        if (roll <= 55) return PublicationType::JournalArticle;
        if (roll <= 67) return PublicationType::ConferencePaper;
        if (roll <= 69) return PublicationType::ConferenceProceedings;
        if (roll <= 71) return PublicationType::JournalVolume;
        if (roll <= 79) return PublicationType::Textbook;
        if (roll <= 83) return PublicationType::Book;
        if (roll <= 85) return PublicationType::BookChapter;
        if (roll <= 88) return PublicationType::Thesis;
        if (roll <= 90) return PublicationType::Dissertation;
        if (roll <= 94) return PublicationType::TechnicalReport;
        if (roll <= 96) return PublicationType::Standard;
        if (roll <= 99) return PublicationType::Preprint;
        return PublicationType::Other;
    }

    Publication makePublication(int index)
    {
        Publication p;
        p.type = pickType();
        p.title = capitalised(words(between(4, 12)));
        if (chance(10))
            p.subtitle = capitalised(words(between(2, 5)));
        if (chance(60))
            p.abstract = capitalised(words(between(30, 90)));
        if (!chance(3))
            p.year = 2026 - int(std::pow(m_random.generateDouble(), 2.0) * 76);
        p.language = pick(kLanguages);

        const int authorCount = between(1, 5);
        for (int a = 0; a < authorCount; ++a)
            p.authors << pickSkewed(m_people);

        switch (p.type) {
        case PublicationType::JournalArticle:
        case PublicationType::JournalVolume:
            p.containerTitle = pickSkewed(m_journals);
            p.volume = QString::number(between(1, 90));
            p.issue = QString::number(between(1, 12));
            p.issn = QStringLiteral("%1-%2").arg(between(1000, 9999)).arg(between(1000, 9999));
            p.doi = QStringLiteral("10.%1/synthetic.%2").arg(between(1000, 9999)).arg(index);
            break;
        case PublicationType::ConferencePaper:
        case PublicationType::ConferenceProceedings:
            p.conferenceName = pickSkewed(m_conferences);
            p.doi = QStringLiteral("10.%1/synthetic.%2").arg(between(1000, 9999)).arg(index);
            break;
        case PublicationType::Textbook:
        case PublicationType::Book:
        case PublicationType::BookChapter:
            p.publisher = pickSkewed(m_publishers);
            p.isbn13 = QStringLiteral("978%1").arg(qint64(1000000000) + index);
            p.edition = QString::number(between(1, 6));
            break;
        case PublicationType::Thesis:
        case PublicationType::Dissertation:
            p.institution = pickSkewed(m_institutions);
            p.degree = p.type == PublicationType::Thesis ? QStringLiteral("MSc") : QStringLiteral("PhD");
            break;
        case PublicationType::TechnicalReport:
            p.institution = pickSkewed(m_institutions);
            p.reportNumber = QStringLiteral("TR-%1").arg(index);
            break;
        case PublicationType::Standard:
            p.standardBody = QStringLiteral("ISO");
            p.standardNumber = QStringLiteral("ISO %1").arg(index);
            break;
        case PublicationType::Preprint:
            p.arxivId = QStringLiteral("26%1.%2").arg(between(10, 12)).arg(index, 5, 10, QChar(u'0'));
            break;
        case PublicationType::Other:
            break;
        }

        const int status = between(1, 100);
        p.readStatus = status <= 70 ? ReadStatus::Unread : status <= 80 ? ReadStatus::Reading : ReadStatus::Read;
        p.needsReview = chance(3);
        return p;
    }

    void addFile(qint64 publicationId, int index)
    {
        FileRecord file;
        file.rootId = pick(m_rootIds);
        file.relativePath = QStringLiteral("folder%1/document-%2.pdf").arg(index % 200).arg(index);
        file.size = between(100000, 90000000);
        file.modifiedAt = 1700000000000LL + index;
        file.format = pick(kFormats);
        file.sha256 = QString::fromLatin1(
            QCryptographicHash::hash(QByteArray::number(index), QCryptographicHash::Sha256).toHex());
        file.pageCount = between(4, 900);
        file.hasTextLayer = !chance(8);

        const int status = between(1, 100);
        file.status = status == 1 ? FileStatus::Missing : status <= 3 ? FileStatus::Offline : FileStatus::Ok;

        m_files.upsert(file);
        m_files.link(publicationId, file.id, true);
    }

    Database &m_db;
    SyntheticOptions m_options;
    QRandomGenerator m_random;
    PublicationRepository m_publications;
    TagRepository m_tags;
    FileRepository m_files;

    QStringList m_journals;
    QStringList m_publishers;
    QStringList m_conferences;
    QStringList m_institutions;
    QList<PersonName> m_people;
    QList<qint64> m_tagIds;
    QList<qint64> m_rootIds;
};

} // namespace

SyntheticStats generateSyntheticLibrary(Database &db, const SyntheticOptions &options)
{
    return Generator(db, options).run();
}

} // namespace papertrail::tools
