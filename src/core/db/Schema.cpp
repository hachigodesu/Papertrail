#include "core/db/Schema.h"

#include "core/db/Database.h"

#include <string>

namespace papertrail::core {

namespace {

// Never edit a released migration: add a new one.
const char *const kMigration1 = R"sql(
CREATE TABLE library_root (
    id          INTEGER PRIMARY KEY,
    uuid        TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    path        TEXT NOT NULL UNIQUE,
    created_at  INTEGER NOT NULL
) STRICT;

CREATE TABLE file (
    id              INTEGER PRIMARY KEY,
    root_id         INTEGER NOT NULL REFERENCES library_root(id) ON DELETE CASCADE,
    relative_path   TEXT NOT NULL,
    file_name       TEXT NOT NULL,
    size            INTEGER NOT NULL,
    modified_at     INTEGER NOT NULL,
    sha256          TEXT,
    format          TEXT NOT NULL,
    page_count      INTEGER,
    has_text_layer  INTEGER CHECK (has_text_layer IN (0, 1)),
    status          TEXT NOT NULL DEFAULT 'ok'
                    CHECK (status IN ('ok', 'missing', 'offline', 'unreadable')),
    UNIQUE (root_id, relative_path)
) STRICT;
CREATE INDEX file_sha256 ON file(sha256);
CREATE INDEX file_size ON file(size);
CREATE INDEX file_name ON file(file_name);
CREATE INDEX file_status ON file(status);
CREATE INDEX file_format ON file(format);

CREATE TABLE publication (
    id                  INTEGER PRIMARY KEY,
    uuid                TEXT NOT NULL UNIQUE,
    type                TEXT NOT NULL CHECK (type IN (
                            'journal_volume', 'journal_article', 'conference_proceedings',
                            'conference_paper', 'textbook', 'book', 'book_chapter', 'thesis',
                            'dissertation', 'technical_report', 'standard', 'preprint', 'other')),
    parent_id           INTEGER REFERENCES publication(id) ON DELETE CASCADE,
    page_start          INTEGER,
    page_end            INTEGER,

    title               TEXT NOT NULL DEFAULT '',
    subtitle            TEXT,
    abstract            TEXT,
    year                INTEGER,
    month               INTEGER CHECK (month BETWEEN 1 AND 12),
    day                 INTEGER CHECK (day BETWEEN 1 AND 31),
    language            TEXT,
    url                 TEXT,
    edition             TEXT,
    series              TEXT,

    doi                 TEXT,
    isbn10              TEXT,
    isbn13              TEXT,
    issn                TEXT,
    eissn               TEXT,
    arxiv_id            TEXT,
    pmid                TEXT,

    container_title     TEXT,
    volume              TEXT,
    issue               TEXT,
    pages               TEXT,
    publisher           TEXT,
    place               TEXT,

    conference_name     TEXT,
    conference_location TEXT,

    institution         TEXT,
    department          TEXT,
    degree              TEXT,

    report_number       TEXT,

    standard_number     TEXT,
    standard_body       TEXT,
    standard_revision   TEXT,
    standard_status     TEXT,

    read_status         TEXT NOT NULL DEFAULT 'unread'
                        CHECK (read_status IN ('unread', 'reading', 'read')),
    rating              INTEGER CHECK (rating BETWEEN 1 AND 5),
    notes               TEXT,
    extra_json          TEXT,

    needs_review        INTEGER NOT NULL DEFAULT 0 CHECK (needs_review IN (0, 1)),

    -- Kept up to date by the application, so sorting and list display need no joins.
    title_sort          TEXT NOT NULL DEFAULT '',
    first_author_sort   TEXT NOT NULL DEFAULT '',
    authors_display     TEXT NOT NULL DEFAULT '',

    created_at          INTEGER NOT NULL,
    updated_at          INTEGER NOT NULL
) STRICT;
CREATE INDEX publication_parent ON publication(parent_id);
CREATE INDEX publication_year ON publication(year);

-- One index per way of sorting a list, with and without a tab's type, so a page never needs
-- to sort the whole library. The coalesce keeps records with no year last in both directions,
-- and has to match the ORDER BY in LibraryQuery.
CREATE INDEX publication_title_sort ON publication(title_sort);
CREATE INDEX publication_first_author_sort ON publication(first_author_sort);
CREATE INDEX publication_year_asc ON publication(coalesce(year, 99999), title_sort);
CREATE INDEX publication_year_desc ON publication(coalesce(year, -1) DESC, title_sort);
CREATE INDEX publication_created ON publication(created_at);
CREATE INDEX publication_type_title ON publication(type, title_sort);
CREATE INDEX publication_type_author ON publication(type, first_author_sort);
CREATE INDEX publication_type_year_asc ON publication(type, coalesce(year, 99999), title_sort);
CREATE INDEX publication_type_year_desc ON publication(type, coalesce(year, -1) DESC, title_sort);
CREATE INDEX publication_type_created ON publication(type, created_at);

CREATE INDEX publication_container ON publication(container_title);
CREATE INDEX publication_publisher ON publication(publisher);
CREATE INDEX publication_conference ON publication(conference_name);
CREATE INDEX publication_institution ON publication(institution);
CREATE INDEX publication_language ON publication(language);
CREATE INDEX publication_read_status ON publication(read_status);
CREATE INDEX publication_needs_review ON publication(needs_review) WHERE needs_review = 1;
CREATE INDEX publication_doi ON publication(doi);
CREATE INDEX publication_isbn13 ON publication(isbn13);
CREATE INDEX publication_arxiv ON publication(arxiv_id);

CREATE TABLE publication_file (
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    file_id         INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    is_primary      INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0, 1)),
    PRIMARY KEY (publication_id, file_id)
) STRICT, WITHOUT ROWID;
CREATE INDEX publication_file_file ON publication_file(file_id, publication_id);

CREATE TABLE person (
    id          INTEGER PRIMARY KEY,
    name_key    TEXT NOT NULL UNIQUE,
    family_name TEXT NOT NULL,
    given_name  TEXT NOT NULL DEFAULT ''
) STRICT;

CREATE TABLE publication_person (
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    person_id       INTEGER NOT NULL REFERENCES person(id),
    role            TEXT NOT NULL CHECK (role IN ('author', 'editor', 'translator', 'advisor')),
    position        INTEGER NOT NULL,
    PRIMARY KEY (publication_id, role, position)
) STRICT, WITHOUT ROWID;
CREATE INDEX publication_person_person ON publication_person(person_id, role, publication_id);

CREATE TABLE tag (
    id          INTEGER PRIMARY KEY,
    uuid        TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    parent_id   INTEGER REFERENCES tag(id) ON DELETE CASCADE,
    colour      TEXT,
    created_at  INTEGER NOT NULL
) STRICT;
-- Two NULL parents don't count as equal in a UNIQUE constraint, so roots need their own index.
CREATE UNIQUE INDEX tag_sibling_name ON tag(parent_id, name COLLATE NOCASE) WHERE parent_id IS NOT NULL;
CREATE UNIQUE INDEX tag_root_name ON tag(name COLLATE NOCASE) WHERE parent_id IS NULL;
CREATE INDEX tag_parent ON tag(parent_id);

CREATE TABLE publication_tag (
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    tag_id          INTEGER NOT NULL REFERENCES tag(id) ON DELETE CASCADE,
    PRIMARY KEY (publication_id, tag_id)
) STRICT, WITHOUT ROWID;
CREATE INDEX publication_tag_tag ON publication_tag(tag_id, publication_id);

CREATE TABLE metadata_provenance (
    id              INTEGER PRIMARY KEY,
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    field           TEXT NOT NULL,
    source          TEXT NOT NULL,
    confidence      REAL,
    previous_value  TEXT,
    new_value       TEXT,
    recorded_at     INTEGER NOT NULL
) STRICT;
CREATE INDEX metadata_provenance_publication ON metadata_provenance(publication_id, field, recorded_at);

CREATE TABLE annotation (
    id              INTEGER PRIMARY KEY,
    uuid            TEXT NOT NULL UNIQUE,
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    content_hash    TEXT,
    location_json   TEXT NOT NULL,
    type            TEXT NOT NULL CHECK (type IN ('highlight', 'underline', 'note')),
    colour          TEXT,
    selected_text   TEXT,
    note            TEXT,
    needs_review    INTEGER NOT NULL DEFAULT 0 CHECK (needs_review IN (0, 1)),
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
) STRICT;
CREATE INDEX annotation_publication ON annotation(publication_id);

CREATE TABLE reading_state (
    publication_id  INTEGER PRIMARY KEY REFERENCES publication(id) ON DELETE CASCADE,
    location        TEXT,
    zoom            REAL,
    last_opened     INTEGER
) STRICT;

CREATE TABLE job (
    id          INTEGER PRIMARY KEY,
    kind        TEXT NOT NULL,
    payload     TEXT NOT NULL,
    status      TEXT NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending', 'running', 'done', 'failed')),
    attempts    INTEGER NOT NULL DEFAULT 0,
    last_error  TEXT,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL
) STRICT;
CREATE INDEX job_status ON job(status, id);

CREATE TABLE setting (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    updated_at  INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

-- Metadata search. Full document text goes in the separate fulltext database (M8).
CREATE VIRTUAL TABLE publication_fts USING fts5(
    title, abstract, authors, tags, container, publisher, identifiers,
    tokenize = "unicode61 remove_diacritics 2"
);

CREATE TRIGGER publication_fts_insert AFTER INSERT ON publication BEGIN
    INSERT INTO publication_fts (rowid, title, abstract, authors, tags, container, publisher, identifiers)
    VALUES (new.id,
            trim(new.title || ' ' || coalesce(new.subtitle, '')),
            new.abstract, '', '',
            coalesce(new.container_title, '') || ' ' || coalesce(new.conference_name, ''),
            coalesce(new.publisher, '') || ' ' || coalesce(new.institution, '') || ' ' || coalesce(new.standard_body, ''),
            concat_ws(' ', new.doi, new.isbn10, new.isbn13, new.issn, new.eissn, new.arxiv_id, new.pmid,
                      new.report_number, new.standard_number));
END;

CREATE TRIGGER publication_fts_update AFTER UPDATE OF
    title, subtitle, abstract, container_title, conference_name, publisher, institution, standard_body,
    doi, isbn10, isbn13, issn, eissn, arxiv_id, pmid, report_number, standard_number
ON publication BEGIN
    UPDATE publication_fts SET
        title = trim(new.title || ' ' || coalesce(new.subtitle, '')),
        abstract = new.abstract,
        container = coalesce(new.container_title, '') || ' ' || coalesce(new.conference_name, ''),
        publisher = coalesce(new.publisher, '') || ' ' || coalesce(new.institution, '') || ' ' || coalesce(new.standard_body, ''),
        identifiers = concat_ws(' ', new.doi, new.isbn10, new.isbn13, new.issn, new.eissn, new.arxiv_id, new.pmid,
                                new.report_number, new.standard_number)
    WHERE rowid = new.id;
END;

CREATE TRIGGER publication_fts_delete AFTER DELETE ON publication BEGIN
    DELETE FROM publication_fts WHERE rowid = old.id;
END;

CREATE TRIGGER publication_person_fts_insert AFTER INSERT ON publication_person BEGIN
    UPDATE publication_fts SET authors = (
        SELECT group_concat(pr.given_name || ' ' || pr.family_name, ' ')
        FROM publication_person pp JOIN person pr ON pr.id = pp.person_id
        WHERE pp.publication_id = new.publication_id
    ) WHERE rowid = new.publication_id;
END;

CREATE TRIGGER publication_person_fts_delete AFTER DELETE ON publication_person BEGIN
    UPDATE publication_fts SET authors = (
        SELECT group_concat(pr.given_name || ' ' || pr.family_name, ' ')
        FROM publication_person pp JOIN person pr ON pr.id = pp.person_id
        WHERE pp.publication_id = old.publication_id
    ) WHERE rowid = old.publication_id;
END;

CREATE TRIGGER publication_tag_fts_insert AFTER INSERT ON publication_tag BEGIN
    UPDATE publication_fts SET tags = (
        SELECT group_concat(t.name, ' ')
        FROM publication_tag pt JOIN tag t ON t.id = pt.tag_id
        WHERE pt.publication_id = new.publication_id
    ) WHERE rowid = new.publication_id;
END;

CREATE TRIGGER publication_tag_fts_delete AFTER DELETE ON publication_tag BEGIN
    UPDATE publication_fts SET tags = (
        SELECT group_concat(t.name, ' ')
        FROM publication_tag pt JOIN tag t ON t.id = pt.tag_id
        WHERE pt.publication_id = old.publication_id
    ) WHERE rowid = old.publication_id;
END;

CREATE TRIGGER tag_rename_fts AFTER UPDATE OF name ON tag BEGIN
    UPDATE publication_fts SET tags = (
        SELECT group_concat(t.name, ' ')
        FROM publication_tag pt JOIN tag t ON t.id = pt.tag_id
        WHERE pt.publication_id = publication_fts.rowid
    ) WHERE rowid IN (SELECT publication_id FROM publication_tag WHERE tag_id = new.id);
END;
)sql";

// M2: why a file couldn't be read, and one queued job at most per file and step.
const char *const kMigration2 = R"sql(
ALTER TABLE file ADD COLUMN problem TEXT;
CREATE UNIQUE INDEX job_one_active ON job(kind, payload) WHERE status IN ('pending', 'running');
)sql";

// M3: preprints became a group of their own, and are recognised from more evidence. Anything that
// might be one is looked at again in the background. Types chosen by the user are left alone then.
const char *const kMigration3 = R"sql(
INSERT OR IGNORE INTO job (kind, payload, created_at, updated_at)
SELECT 'reclassify', json_object('file', f.id), unixepoch() * 1000, unixepoch() * 1000
FROM file f
JOIN publication_file pf ON pf.file_id = f.id AND pf.is_primary = 1
JOIN publication p ON p.id = pf.publication_id
WHERE f.status = 'ok' AND f.sha256 IS NOT NULL
  AND p.type IN ('journal_article', 'conference_paper', 'technical_report', 'other');
)sql";

// Automatic sorting into preprints put too many published papers there, so it's been removed, and
// its results are undone. A preprint is now only ever something the user has marked as one.
//
// Anything the check from migration 3 moved goes straight back. Anything sorted at import, under the
// earlier rule, has no earlier type to go back to, so it's queued for working out again.
const char *const kMigration4 = R"sql(
DELETE FROM job WHERE kind = 'reclassify';

CREATE TEMP TABLE undo_sorting AS
SELECT p.id AS id,
       (SELECT mp.previous_value FROM metadata_provenance mp
        WHERE mp.publication_id = p.id AND mp.field = 'type' AND mp.source = 'classifier'
          AND mp.new_value = 'preprint'
        ORDER BY mp.recorded_at DESC, mp.id DESC LIMIT 1) AS earlier
FROM publication p
WHERE p.type = 'preprint'
  AND NOT EXISTS (SELECT 1 FROM metadata_provenance mp
                  WHERE mp.publication_id = p.id AND mp.field = 'type' AND mp.source = 'manual');

INSERT INTO metadata_provenance (publication_id, field, source, previous_value, new_value, recorded_at)
SELECT id, 'type', 'classifier', 'preprint', earlier, unixepoch() * 1000 FROM undo_sorting WHERE earlier IS NOT NULL;

UPDATE publication
SET type = (SELECT earlier FROM undo_sorting u WHERE u.id = publication.id), updated_at = unixepoch() * 1000
WHERE id IN (SELECT id FROM undo_sorting WHERE earlier IS NOT NULL);

INSERT OR IGNORE INTO job (kind, payload, created_at, updated_at)
SELECT 'reclassify', json_object('file', pf.file_id), unixepoch() * 1000, unixepoch() * 1000
FROM undo_sorting u
JOIN publication_file pf ON pf.publication_id = u.id AND pf.is_primary = 1
WHERE u.earlier IS NULL;

DROP TABLE undo_sorting;
)sql";

// One publication can have several formats. Looks through the library once, for any already imported
// as separate publications. Only worth doing if there's more than one publication to compare.
const char *const kMigration5 = R"sql(
CREATE INDEX IF NOT EXISTS publication_isbn10 ON publication(isbn10);

INSERT OR IGNORE INTO job (kind, payload, created_at, updated_at)
SELECT 'join_formats', '{}', unixepoch() * 1000, unixepoch() * 1000
WHERE (SELECT count(*) FROM publication) > 1;
)sql";

// KF8 Kindle books can now be read, so ones already in the library can have their covers and page
// counts. Publications are left as they are: only the file is looked at again.
const char *const kMigration6 = R"sql(
INSERT OR IGNORE INTO job (kind, payload, created_at, updated_at)
SELECT 'inspect', json_object('file', id), unixepoch() * 1000, unixepoch() * 1000
FROM file
WHERE format IN ('azw3', 'mobi') AND sha256 IS NOT NULL AND page_count IS NULL AND status = 'ok';
)sql";

// Online lookups: what's been fetched, so that a source isn't asked the same thing twice, and what's
// waiting for the user to decide on.
const char *const kMigration7 = R"sql(
CREATE TABLE IF NOT EXISTS lookup_cache (
    key         TEXT PRIMARY KEY,
    status      INTEGER NOT NULL,
    body        TEXT NOT NULL,
    fetched_at  INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS lookup_candidate (
    id              INTEGER PRIMARY KEY,
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    provider        TEXT NOT NULL,
    matched_by      TEXT NOT NULL,
    score           REAL NOT NULL,
    record          TEXT NOT NULL,
    created_at      INTEGER NOT NULL
) STRICT;
CREATE INDEX IF NOT EXISTS lookup_candidate_publication ON lookup_candidate(publication_id);
)sql";

// Removing a folder used to delete its file records, which left its publications showing with no file,
// and with nothing to reconnect them if the folder came back. Now a removed folder keeps its records,
// and its publications are hidden until the folder returns or the user deletes them.
//
// hidden_publication is a table of its own because the library's queries can then rule these out from
// their indexes alone, without reading each publication. A publication with no file isn't necessarily
// hidden: one imported from another computer may be waiting for its file to turn up.
//
// Before this, only removing a folder could leave a publication with no file, so those are hidden too.
const char *const kMigration8 = R"sql(
CREATE TABLE IF NOT EXISTS removed_root (
    root_id     INTEGER PRIMARY KEY REFERENCES library_root(id) ON DELETE CASCADE,
    removed_at  INTEGER NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS hidden_publication (
    publication_id  INTEGER PRIMARY KEY REFERENCES publication(id) ON DELETE CASCADE
) STRICT;

INSERT OR IGNORE INTO hidden_publication (publication_id)
SELECT p.id FROM publication p
WHERE coalesce(p.parent_id, p.id) NOT IN (SELECT publication_id FROM publication_file);

-- A hidden publication that gets a real file is back in the library, however that came about. A copy
-- it had in a removed folder is no longer of use, and the new file takes over if that was the main one.
CREATE TRIGGER IF NOT EXISTS publication_file_reveals AFTER INSERT ON publication_file
BEGIN
    DELETE FROM hidden_publication
    WHERE publication_id = NEW.publication_id
       OR publication_id IN (SELECT id FROM publication WHERE parent_id = NEW.publication_id);
    DELETE FROM file
    WHERE root_id IN (SELECT root_id FROM removed_root)
      AND id IN (SELECT file_id FROM publication_file WHERE publication_id = NEW.publication_id);
    UPDATE publication_file SET is_primary = 1
    WHERE publication_id = NEW.publication_id AND file_id = NEW.file_id
      AND NOT EXISTS (SELECT 1 FROM publication_file o WHERE o.publication_id = NEW.publication_id AND o.is_primary = 1);
END;
)sql";

// Collections are groupings the user makes, such as for a project. A publication can be in any number.
const char *const kMigration9 = R"sql(
CREATE TABLE IF NOT EXISTS collection (
    id          INTEGER PRIMARY KEY,
    uuid        TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL COLLATE NOCASE UNIQUE,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS collection_publication (
    collection_id   INTEGER NOT NULL REFERENCES collection(id) ON DELETE CASCADE,
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    added_at        INTEGER NOT NULL,
    PRIMARY KEY (collection_id, publication_id)
) STRICT, WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS collection_publication_publication ON collection_publication(publication_id, collection_id);
)sql";

// A summary the user chose to keep, apart from the publication's details and notes, since it's generated.
const char *const kMigration10 = R"sql(
CREATE TABLE IF NOT EXISTS publication_summary (
    id                      INTEGER PRIMARY KEY,
    uuid                    TEXT NOT NULL UNIQUE,
    publication_id          INTEGER NOT NULL UNIQUE REFERENCES publication(id) ON DELETE CASCADE,
    model                   TEXT NOT NULL,
    effort                  TEXT NOT NULL,
    basis                   TEXT NOT NULL,
    identification_unsure   INTEGER NOT NULL DEFAULT 0,
    summary_text            TEXT NOT NULL,
    sources_json            TEXT NOT NULL,
    cost_dollars            REAL,
    sent_paper              INTEGER NOT NULL DEFAULT 0,
    created_at              INTEGER NOT NULL
) STRICT;
)sql";

// For a library imported from another computer: the files each publication had there, by contents, so
// each can be connected when a file with those contents turns up here. Kept until it does.
//
// And possible matches for those that weren't certain, such as another download of the same paper, which
// wait for the user to accept or reject them.
const char *const kMigration11 = R"sql(
CREATE TABLE IF NOT EXISTS expected_file (
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    sha256          TEXT NOT NULL,
    size            INTEGER,
    format          TEXT,
    file_name       TEXT,
    is_primary      INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0, 1)),
    PRIMARY KEY (publication_id, sha256)
) STRICT, WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS expected_file_sha256 ON expected_file(sha256);

CREATE TABLE IF NOT EXISTS file_proposal (
    id              INTEGER PRIMARY KEY,
    publication_id  INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE,
    file_id         INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    reason          TEXT NOT NULL CHECK (reason IN ('identifier', 'name_and_size')),
    created_at      INTEGER NOT NULL,
    UNIQUE (publication_id, file_id)
) STRICT;
CREATE INDEX IF NOT EXISTS file_proposal_file ON file_proposal(file_id);
)sql";

// Publications imported from another computer stay out of sight until their files are found here, as those
// kept for a removed folder do. The trigger that brings a hidden publication back when it gets a file does the
// rest. Libraries that imported before this get theirs hidden now.
const char *const kMigration12 = R"sql(
INSERT OR IGNORE INTO hidden_publication (publication_id)
SELECT p.id FROM publication p WHERE coalesce(p.parent_id, p.id) IN (SELECT publication_id FROM expected_file) AND coalesce(p.parent_id, p.id) NOT IN (SELECT publication_id FROM publication_file);
)sql";

} // namespace

const std::vector<Migration> &libraryMigrations()
{
    static const std::vector<Migration> migrations = {
        {1, kMigration1},
        {2, kMigration2},
        {3, kMigration3},
        {4, kMigration4},
        {5, kMigration5},
        {6, kMigration6},
        {7, kMigration7},
        {8, kMigration8},
        {9, kMigration9},
        {10, kMigration10},
        {11, kMigration11},
        {12, kMigration12},
    };
    return migrations;
}

void migrate(Database &db, const std::vector<Migration> &migrations)
{
    int current = db.userVersion();
    const int newest = migrations.empty() ? 0 : migrations.back().version;
    if (current > newest) {
        throw DbError(0, "this library was created by a newer version of Papertrail (schema "
                             + std::to_string(current) + ", this version supports "
                             + std::to_string(newest) + ")");
    }

    for (const Migration &migration : migrations) {
        if (migration.version <= current)
            continue;
        Transaction transaction(db);
        db.exec(migration.sql);
        db.setUserVersion(migration.version);
        transaction.commit();
        current = migration.version;
    }
}

void migrateLibrary(Database &db)
{
    migrate(db, libraryMigrations());
}

} // namespace papertrail::core
