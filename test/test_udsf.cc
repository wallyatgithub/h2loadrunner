#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "udsf_data_store.h"

auto& realm = udsf::get_realm("realm1");
auto& storage = realm.get_storage("storage1");
std::vector<std::string> record_names;
std::vector<std::string> multipartBodies;
size_t number_records = 1000;

void test_run_search_expression()
{
    std::string json =
        R"(
{
  "cond": "AND",
  "units": [
    {
      "op": "GTE",
      "tag": "ueId",
      "value": "0"
    },
    {
      "recordIdList": [
        "record1",
        "record2",
        "record3"
      ]
    },
    {
      "cond": "AND",
      "units": [
        {
          "op": "LTE",
          "tag": "ueId",
          "value": "99999999"
        },
        {
          "recordIdList": [
            "record1",
            "record2",
            "record4"
          ]
        },
        {
          "cond": "NOT",
          "units": [
            {
              "op": "EQ",
              "tag": "ueId",
              "value": "123456"
            }
          ]
        }
      ]
    }
  ]
}
)";

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

    auto record_id1 = std::string("record1");
    std::string multipartBody1;
    multipartBody1 += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody1 += "Content-Disposition: json-data; name=\"first_part\"\r\n";
    multipartBody1 += "Content-id: meta\r\n";
    multipartBody1 += "Content-Type: application/json\r\n";
    multipartBody1 += "\r\n";
    multipartBody1 += "{\"schemaId\": \"schema_1\",\"tags\":{\"ueId\" : [ \"";
    multipartBody1 += std::to_string(455345);
    multipartBody1 += "\", \"";
    multipartBody1 += std::to_string(455345 + 1);
    multipartBody1 += "\" ], \"recordId\" : [ \"";
    multipartBody1 += std::to_string(30001);
    multipartBody1 += "\" ] }}\r\n";
    multipartBody1 += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody1 += "Content-Disposition: form-data; name=\"first_part\"\r\n";
    multipartBody1 += "Content-id: block1\r\n";
    multipartBody1 += "Content-Type: text\r\n";
    multipartBody1 += "\r\n";
    multipartBody1 += "This is the first part of the HTTP body.\r\n";
    multipartBody1 += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody1 += "Content-Disposition: form-data; name=\"second_part\"\r\n";
    multipartBody1 += "Content-id: block2\r\n";
    multipartBody1 += "Content-Type: text\r\n";
    multipartBody1 += "\r\n";
    multipartBody1 += "This is the second part of the HTTP body.\r\n";
    multipartBody1 += "-----MULTIPART_BOUNDARY-----\r\n";

    storage.insert_or_update_record(record_id1, "-----MULTIPART_BOUNDARY---", multipartBody1);

    auto record_id2 = std::string("record2");
    std::string multipartBody2;
    multipartBody2 += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody2 += "Content-Disposition: json-data; name=\"first_part\"\r\n";
    multipartBody2 += "Content-id: meta\r\n";
    multipartBody2 += "Content-Type: application/json\r\n";
    multipartBody2 += "\r\n";
    multipartBody2 += "{\"schemaId\": \"schema_1\",\"tags\":{\"ueId\" : [ \"";
    multipartBody2 += std::to_string(65676767);
    multipartBody2 += "\", \"";
    multipartBody2 += std::to_string(65676767 + 1);
    multipartBody2 += "\" ], \"recordId\" : [ \"";
    multipartBody2 += std::to_string(30002);
    multipartBody2 += "\" ] }}\r\n";
    multipartBody2 += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody2 += "Content-Disposition: form-data; name=\"first_part\"\r\n";
    multipartBody2 += "Content-id: block1\r\n";
    multipartBody2 += "Content-Type: text\r\n";
    multipartBody2 += "\r\n";
    multipartBody2 += "This is the first part of the HTTP body.\r\n";
    multipartBody2 += "-----MULTIPART_BOUNDARY---\r\n";
    multipartBody2 += "Content-Disposition: form-data; name=\"second_part\"\r\n";
    multipartBody2 += "Content-id: block2\r\n";
    multipartBody2 += "Content-Type: text\r\n";
    multipartBody2 += "\r\n";
    multipartBody2 += "This is the second part of the HTTP body.\r\n";
    multipartBody2 += "-----MULTIPART_BOUNDARY-----\r\n";

    storage.insert_or_update_record(record_id2, "-----MULTIPART_BOUNDARY---", multipartBody2);

    auto r = storage.get_all_record_ids("schema_1");
    for (auto& s: r)
    {
        std::cout<<"all record IDs: "<<s<<std::endl;
    }   

    std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    auto ret = storage.run_search_expression({}, "", doc);

    for (auto& s: ret)
    {
        std::cout<<"record IDs in set: "<<s<<std::endl;
    }
}


void add_record()
{
    auto before_timepoint = std::chrono::steady_clock::now();
    for (size_t i = 0; i < number_records; i++)
    {
        auto ret = storage.insert_or_update_record(record_names[i], "-----MULTIPART_BOUNDARY---", multipartBodies[i]);
        //std::cout << "insert_or_update_record ret:" << ret << std::endl;
        //storage.update_record_meta(record_names[i],
        //                           R"([{ "op": "replace", "path": "/tags/ueId", "value": ["450005"] }, { "op": "remove", "path": "/tags/recordId" }])");
    //std::cout << "record:" << storage.get_record("record1") << std::endl;
    //storage.get_record(record_names[i]);
    //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;
}
auto after_timepoint = std::chrono::steady_clock::now();

std::cout << "each record add cost:" << std::chrono::duration_cast<std::chrono::microseconds>
          (after_timepoint - before_timepoint).count() / number_records << std::endl;
std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1",
                                                                                        "recordId") << std::endl;
}

void get_record()
{
    auto before_timepoint = std::chrono::steady_clock::now();
    for (size_t i = 0; i < number_records; i++)
    {
        storage.get_record(record_names[i]);
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    std::cout << "each record get cost:" << std::chrono::duration_cast<std::chrono::microseconds>
              (after_timepoint - before_timepoint).count() / number_records << std::endl;
    std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1",
                                                                                            "recordId") << std::endl;
}

void delete_record()
{
    //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;;
    auto before_timepoint = std::chrono::steady_clock::now();
    for (size_t i = 0; i < number_records; i++)
    {
        auto ret = storage.delete_record(record_names[i]);
        //std::cout<<"delete record ret:"<<ret<<std::endl;
        //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", record_names[i]) << std::endl;;
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    std::cout << "each record delete cost:" << std::chrono::duration_cast<std::chrono::microseconds>
              (after_timepoint - before_timepoint).count() / number_records << std::endl;
    std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1",
                                                                                            "recordId") << std::endl;;
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

    for (size_t i = 0; i < number_records; i++)
    {
        record_names.emplace_back(std::string("record").append(std::to_string(i)));
        std::string multipartBody;
        multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
        multipartBody += "Content-Disposition: json-data; name=\"first_part\"\r\n";
        multipartBody += "Content-id: meta\r\n";
        multipartBody += "Content-Type: application/json\r\n";
        multipartBody += "\r\n";
        multipartBody += "{\"schemaId\": \"schema_1\",\"tags\":{\"ueId\" : [ \"";
        multipartBody += std::to_string(10000+i);
        multipartBody += "\", \"";
        multipartBody += std::to_string(20000+i);
        multipartBody += "\" ], \"recordId\" : [ \"";
        multipartBody += std::to_string(30000+i);
        multipartBody += "\" ] }}\r\n";
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
        multipartBodies.emplace_back(std::move(multipartBody));
    }

    storage.add_or_update_schema("schema_1", schema);

    //std::thread add_thread(add_record);
    //std::thread delete_thread(delete_record);
    //add_thread.join();
    //delete_thread.join();
    add_record();
    get_record();
    delete_record();
    test_run_search_expression();
    return 0;
}
