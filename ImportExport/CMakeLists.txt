if(ENABLE_AWS_S3)
  include_directories(${LibAwsS3_INCLUDE_DIRS})
  list(APPEND IMPORT_EXPORT_LIBRARIES "${LibAwsS3_LIBRARIES}")
  list(APPEND S3Archive ../Archive/S3Archive.cpp)
endif()

if(ENABLE_IMPORT_PARQUET)
  list(APPEND IMPORT_EXPORT_LIBRARIES "${Parquet_LIBRARIES}")
endif()

list(APPEND IMPORT_EXPORT_LIBRARIES muparserx)

set(IMPORT_SOURCES
  Importer.cpp
  ForeignDataImporter.cpp
  DelimitedParserUtils.cpp
  RasterImporter.cpp
  MetadataColumn.cpp
  ExpressionParser.cpp)

set(EXPORT_SOURCES
  QueryExporter.cpp
  QueryExporterCSV.cpp
  QueryExporterGDAL.cpp)

add_library(ImportExport ${IMPORT_SOURCES} ${EXPORT_SOURCES} ${S3Archive})

find_package(XercesC REQUIRED)

target_link_libraries(ImportExport mapd_thrift Logger Shared Catalog DataMgr Geospatial StringDictionary ${GDAL_LIBRARIES} ${CMAKE_DL_LIBS} ${XercesC_LIBRARIES} ${LibArchive_LIBRARIES} ${IMPORT_EXPORT_LIBRARIES} ${Arrow_LIBRARIES})

add_library(RowToColumn RowToColumnLoader.cpp RowToColumnLoader.h DelimitedParserUtils.cpp DelimitedParserUtils.h)
target_link_libraries(RowToColumn ThriftClient)
add_dependencies(RowToColumn mapd_thrift)

add_executable(StreamImporter StreamImporter.cpp)
target_link_libraries(StreamImporter RowToColumn mapd_thrift Logger Shared ${CMAKE_DL_LIBS} ${Boost_LIBRARIES} ${PROFILER_LIBS})

add_executable(KafkaImporter KafkaImporter.cpp)
target_link_libraries(KafkaImporter RowToColumn mapd_thrift ${RdKafka_LIBRARIES} Logger Shared ${CMAKE_DL_LIBS} ${Boost_LIBRARIES} ${PROFILER_LIBS})

install(TARGETS StreamImporter KafkaImporter DESTINATION bin  COMPONENT "exe")
