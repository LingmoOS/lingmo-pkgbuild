file(GLOB PO_FILES *.po)

foreach(po ${PO_FILES})
    get_filename_component(lang ${po} NAME_WE)
    gettext_process_po_files(${lang} ALL PO_FILES ${po})
endforeach() 