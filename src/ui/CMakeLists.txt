add_library(papertrail_ui STATIC
    BulkChanges.cpp
    BulkChanges.h
    CoverCache.cpp
    CoverCache.h
    Delegates.cpp
    Delegates.h
    DetailsPanel.cpp
    DetailsPanel.h
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
    MainWindow.cpp
    MainWindow.h
    MetadataEditor.cpp
    MetadataEditor.h
    PublicationListModel.cpp
    PublicationListModel.h
    QueryThreads.cpp
    QueryThreads.h)

target_include_directories(papertrail_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_ui PUBLIC papertrail_core papertrail_import Qt6::Widgets)
