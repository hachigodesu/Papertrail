add_library(papertrail_ui STATIC
    BulkChanges.cpp
    BulkChanges.h
    CoverCache.cpp
    CoverCache.h
    Delegates.cpp
    Delegates.h
    DetailsPanel.cpp
    DetailsPanel.h
    DocumentClient.cpp
    DocumentClient.h
    FilterSidebar.cpp
    FilterSidebar.h
    FolderWatcher.cpp
    FolderWatcher.h
    ImportThread.cpp
    ImportThread.h
    Labels.cpp
    Labels.h
    LibraryFoldersDialog.cpp
    LibraryFoldersDialog.h
    LibrarySession.cpp
    LibrarySession.h
    LookupThread.cpp
    LookupThread.h
    MainWindow.cpp
    MainWindow.h
    MetadataEditor.cpp
    MarkedCopy.cpp
    MarkedCopy.h
    MetadataEditor.h
    SummariesDialog.cpp
    SummariesDialog.h
    OnlineLookupDialog.cpp
    OnlineLookupDialog.h
    PageView.cpp
    PageView.h
    ReferencePreview.cpp
    ReferencePreview.h
    PublicationListModel.cpp
    PublicationListModel.h
    QueryThreads.cpp
    QueryThreads.h
    ReaderPair.cpp
    ReferenceImportDialog.cpp
    ReferenceImportDialog.h
    RelatedPapersDialog.cpp
    RelatedPapersDialog.h
    ReaderPair.h
    ReaderTab.cpp
    ReaderTab.h
    IndexThread.cpp
    IndexThread.h
    ArticleCoverThread.cpp
    ArticleCoverThread.h
    BackupsDialog.cpp
    BackupsDialog.h
    BatchSummaryDialog.cpp
    BatchSummaryDialog.h
    CitationActions.cpp
    CitationActions.h
    ContentsPopup.cpp
    ContentsPopup.h
    GridView.cpp
    GridView.h
    TransferDialogs.cpp
    TransferDialogs.h
    ApiKeyStore.cpp
    ApiKeyStore.h
    HighlightsWindow.cpp
    HighlightsWindow.h
    KeyField.cpp
    KeyField.h
    LibraryCheckDialog.cpp
    LibraryCheckDialog.h
    SummaryDialog.cpp
    SummaryDialog.h
    VolumeDialogs.cpp
    VolumeDialogs.h
    ReviewTab.cpp
    ReviewTab.h
    SettingsDialog.cpp
    SettingsDialog.h
    Speaker.cpp
    Speaker.h
    Theme.cpp
    Theme.h)

target_include_directories(papertrail_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_ui PUBLIC papertrail_summary papertrail_citation papertrail_transfer Papertrail::Keychain papertrail_core papertrail_import papertrail_metadata Qt6::Widgets)
# The English model is found in the source until it's installed beside the program.
target_compile_definitions(papertrail_ui PRIVATE PAPERTRAIL_SOURCE_TESSDATA="${CMAKE_SOURCE_DIR}/packaging/tessdata"
                                                 PAPERTRAIL_BUILD_DIR="${CMAKE_BINARY_DIR}")
if(PAPERTRAIL_WITH_OCR)
    target_compile_definitions(papertrail_ui PRIVATE PAPERTRAIL_WITH_OCR)
endif()

# Reading aloud. On Linux, Papertrail registers flite's US English voice itself, since Qt's flite engine only looks for
# voices in the system's own folders, never where a Flatpak or AppImage keeps them. See docs/1.4.0-speech.md.
if(PAPERTRAIL_WITH_SPEECH)
    target_link_libraries(papertrail_ui PRIVATE Qt6::TextToSpeech Qt6::Multimedia)
    target_compile_definitions(papertrail_ui PRIVATE PAPERTRAIL_WITH_SPEECH)
    if(UNIX AND NOT APPLE)
        find_path(FLITE_INCLUDE_DIR flite/flite.h)
        find_library(FLITE_LIBRARY flite)
        find_library(FLITE_VOICE_LIBRARY flite_cmu_us_slt)
        find_library(FLITE_LANGUAGE_LIBRARY flite_usenglish)
        find_library(FLITE_LEXICON_LIBRARY flite_cmulex)
        if(NOT FLITE_INCLUDE_DIR OR NOT FLITE_LIBRARY OR NOT FLITE_VOICE_LIBRARY OR NOT FLITE_LANGUAGE_LIBRARY OR NOT FLITE_LEXICON_LIBRARY)
            message(FATAL_ERROR "Reading aloud needs flite and its US English voice (flite1-dev, or flite), "
                                "or configure with -DPAPERTRAIL_WITH_SPEECH=OFF")
        endif()
        target_include_directories(papertrail_ui PRIVATE ${FLITE_INCLUDE_DIR})
        target_link_libraries(papertrail_ui PRIVATE ${FLITE_VOICE_LIBRARY} ${FLITE_LANGUAGE_LIBRARY} ${FLITE_LEXICON_LIBRARY} ${FLITE_LIBRARY})
        target_compile_definitions(papertrail_ui PRIVATE PAPERTRAIL_WITH_FLITE)
    endif()
endif()
