#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "udsf_data_store.h"

auto& realm = udsf::get_realm("realm1");
auto& storage = realm.get_storage("storage1");

void add_record()
{
    std::string multipartBody;
    multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody += "Content-Disposition: json-data; name=\"first_part\"\r\n";
    multipartBody += "Content-id: meta\r\n";
    multipartBody += "Content-Type: application/json\r\n";
    multipartBody += "\r\n";
    multipartBody +=
        "{\"schemaId\": \"schema_1\",\"tags\":{\"ueId\" : [ \"455345\", \"455346\" ], \"recordId\" : [ \"1000106\" ] }}\r\n";
    multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody += "Content-Disposition: form-data; name=\"first_part\"\r\n";
    multipartBody += "Content-id: block1\r\n";
    multipartBody += "Content-Type: text\r\n";
    multipartBody += "\r\n";
    multipartBody += "This is the first part of the HTTP body.\r\n";
    multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody += "Content-Disposition: form-data; name=\"second_part\"\r\n";
    multipartBody += "Content-id: block2\r\n";
    multipartBody += "Content-Type: text\r\n";
    multipartBody += "\r\n";
    multipartBody += "This is the second part of the HTTP body.\r\n";
    multipartBody += "-----MULTIPART_BOUNDARY-----\r\n";

    std::vector<std::string> record_names;
    size_t number_records = 10000;
    for (size_t i = 0; i < number_records; i++)
    {
        record_names.emplace_back(std::string("record").append(std::to_string(i)));
    }

    auto before_timepoint = std::chrono::steady_clock::now();
    for (size_t i = 0; i < number_records; i++)
    {
        auto ret = storage.insert_or_update_record(record_names[i], "-----MULTIPART_BOUNDARY---", multipartBody);
        //std::cout << "insert_or_update_record ret:" << ret << std::endl;
        storage.update_record_meta(record_names[i],
                                   R"([{ "op": "replace", "path": "/tags/ueId", "value": ["450005"] }, { "op": "remove", "path": "/tags/recordId" }])");
        //std::cout << "record:" << storage.get_record("record1") << std::endl;
        storage.get_record(record_names[i]);
        //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;;
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    std::cout << "each record add cost:" << std::chrono::duration_cast<std::chrono::microseconds>
              (after_timepoint - before_timepoint).count() / number_records << std::endl;
}

void delete_record()
{
    std::vector<std::string> record_names;
    size_t number_records = 100;
    for (size_t i = 0; i < number_records; i++)
    {
        record_names.emplace_back(std::string("record").append(std::to_string(i)));
    }
    auto before_timepoint = std::chrono::steady_clock::now();
    for (size_t i = 0; i < number_records; i++)
    {
        auto ret = storage.delete_record(record_names[i]);
        //std::cout<<"delete record ret:"<<ret<<std::endl;
        //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;;
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    std::cout << "each record delete cost:" << std::chrono::duration_cast<std::chrono::microseconds>
              (after_timepoint - before_timepoint).count() / number_records << std::endl;
}

int main(void)
{
    auto schema = R"(
{
  "schemaId": "schema_1",
  "metaTags": [
    {
      "tagName": "ueId",
      "keyType": "UNIQUE_KEY",
      "sort": true,
      "presence": true
    },
    {
      "tagName": "hashId",
      "keyType": "UNIQUE_KEY",
      "sort": false,
      "presence": false
    },
    {
      "tagName": "recordId",
      "keyType": "UNIQUE_KEY",
      "sort": false,
      "presence": true
    }
  ]
}
)";
    storage.add_or_update_schema("schema_1", schema);
    std::thread add_thread(add_record);
    std::thread delete_thread(delete_record);
    add_thread.join();
    delete_thread.join();
    //add_record();
    //delete_record();
    return 0;
}