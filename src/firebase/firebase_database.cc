#include "esp_cxx/firebase/firebase_database.h"
#include "esp_cxx/httpd/mongoose_event_manager.h"

#include <array>

#include "esp_cxx/logging.h"

#include "cJSON.h"
extern "C" {
#include "cJSON_Utils.h"
}

namespace {

struct RemoveEmptyState {
  cJSON* parent;
  cJSON* cur;
  cJSON* entry;
};

}  // namespace

namespace esp_cxx {

FirebaseDatabase::FirebaseDatabase(
    const std::string& host,
    const std::string& database,
    const std::string& listen_path,
    MongooseEventManager* event_manager)
  : host_(host),
    database_(database),
    listen_path_(listen_path),
    event_manager_(event_manager),
    websocket_(event_manager_,
               "wss://" + host_ + "/.ws?v=5&ns=" + database_),
    root_(cJSON_CreateObject()),
    update_template_(cJSON_CreateObject()) {
}

FirebaseDatabase::~FirebaseDatabase() {
}

void FirebaseDatabase::Connect() {
  websocket_.Connect<FirebaseDatabase, &FirebaseDatabase::OnWsFrame>(this);
  SendKeepalive();
}

void FirebaseDatabase::SetUpdateHandler(std::function<void(void)> on_update) {
  on_update_ = on_update;
}

void FirebaseDatabase::Publish(const std::string& path,
                               unique_cJSON_ptr new_value) {
  // Example packet:
  //  {"t":"d","d":{"r":4,"a":"p","b":{"p":"/test","d":{"hi":"mom","num":1547104593160},"h":""}}}

  // Create data envlope.
  unique_cJSON_ptr publish(cJSON_CreateObject());
  cJSON_AddStringToObject(publish.get(), "t", "d");
  cJSON* data = cJSON_AddObjectToObject(publish.get(), "d");

  // Create publish request
  cJSON_AddNumberToObject(data, "r", ++request_num_);
  cJSON_AddStringToObject(data, "a", "p");
  cJSON* body = cJSON_AddObjectToObject(data, "b");

  // Insert data.
  cJSON_AddStringToObject(body, "p", path.c_str());
  cJSON_AddItemToObject(body, "p", cJSON_Duplicate(new_value.get(), true));

  ReplacePath(path.c_str(), std::move(new_value));

  websocket_.SendText(cJSON_PrintUnformatted(publish.get()));
}

cJSON* FirebaseDatabase::Get(const std::string& path) {
  cJSON* parent;
  cJSON* node;
  GetPath(path, &parent, &node);
  return node;
}

void FirebaseDatabase::GetPath(const std::string& path, cJSON** parent_out, cJSON** node_out,
                               bool create_path, std::string* last_key_out) {
  static constexpr char kPathSeparator[] = "/";

  cJSON* parent = nullptr;
  cJSON* cur = root_.get();

  unique_C_ptr<char> path_copy(strdup(path.c_str()));
  const char* last_key = nullptr;
  for (const char* key = strtok(path_copy.get(), kPathSeparator);
       key;
       key = strtok(NULL, kPathSeparator)) {
    last_key = key;
    parent = cur;
    cur = cJSON_GetObjectItemCaseSensitive(parent, key);

    // does not exist.
    if (create_path && !cur) {
      // If node is null, just start creating the parents.
      cur = cJSON_AddObjectToObject(parent, key);
    }
  }

  *parent_out = parent;
  *node_out = cur;
  if (last_key_out && last_key) {
    *last_key_out = last_key;
  }
}

bool FirebaseDatabase::RemoveEmptyNodes(cJSON* node) {
  if (!cJSON_IsObject(node))
    return true;

  std::array<RemoveEmptyState, 10> stack; // No more than 10 deep please.
  int stack_level = 0;
  stack.at(stack_level++) = {nullptr, node, node->child};

  while (stack_level > 0) {
    RemoveEmptyState state = stack.at(--stack_level);
    cJSON* entry = state.entry;
    while (entry) {
      if (cJSON_IsNull(entry)) {
        cJSON_Delete(cJSON_DetachItemViaPointer(state.cur, entry));
      } else if (cJSON_IsObject(entry)) {
        if (cJSON_GetArraySize(state.cur) == 0) {
          cJSON_Delete(cJSON_DetachItemViaPointer(state.parent, state.cur));
        } else {
          if (stack_level + 2 >= stack.size()) {
            return false;
          }
          stack.at(stack_level++) = {state.parent, state.cur, entry->next};
          stack.at(stack_level++) = {state.cur, entry, entry->child};
          goto recur;
        }
      }
      entry = entry->next;
    }

recur:
    ;
  }

  return true;
}

void FirebaseDatabase::OnWsFrame(WebsocketFrame frame) {
  switch(frame.opcode()) {
    case WebsocketOpcode::kBinary:
      break;

    case WebsocketOpcode::kText: {
      unique_cJSON_ptr json(cJSON_Parse(frame.data().data()));
      OnCommand(json.get());
      if (on_update_) {
        on_update_();
      }
      break;
    }

    case WebsocketOpcode::kPing:
      // Pong is already sent by mongoose. This is just a notification.
    case WebsocketOpcode::kPong:
      break;

    case WebsocketOpcode::kClose:
      // TODO(awong): Invalidate socket. Reconnect.
      break;

    case WebsocketOpcode::kContinue:
      // TODO(awong): Shouldn't be here. The mongose implementation is supposed to reconstruct.
      break;
  }
}

void FirebaseDatabase::OnCommand(cJSON* command) {
  // Find the envelope.
  // Dispatch update.
  cJSON* type = cJSON_GetObjectItemCaseSensitive(command, "t");
  if (cJSON_IsString(type)) {
    cJSON* data = cJSON_GetObjectItemCaseSensitive(command, "d");

    // Type has 2 possibilities:
    //   c = connection oriented command like server information or
    //       redirect info.
    //   d = data commands such as publishing new database entries.
    if (strcmp(type->valuestring, "c") == 0) {
      OnConnectionCommand(data);
    } else if (strcmp(type->valuestring, "d") == 0) {
      OnDataCommand(data);
    }
  }
}

void FirebaseDatabase::OnConnectionCommand(cJSON* command) {
  cJSON* type = cJSON_GetObjectItemCaseSensitive(command, "t");
  cJSON* data = cJSON_GetObjectItemCaseSensitive(command, "d");
  cJSON* host = cJSON_GetObjectItemCaseSensitive(data, "h");

  // Two types of connection requests
  //   h - host data
  //   r - redirect.
  if (cJSON_IsString(type) && cJSON_IsString(host) && host->valuestring != nullptr) {
    real_host_ = host->valuestring;
    if (strcmp(type->valuestring, "h") == 0) {
      // TODO(ajwong): Print other data? maybe?
    } else if (strcmp(type->valuestring, "r") == 0) {
      // TODO(awong): Reconnect.
    }
  }
}

void FirebaseDatabase::OnDataCommand(cJSON* command) {
  cJSON* request_id = cJSON_GetObjectItemCaseSensitive(command, "r");
  cJSON* action = cJSON_GetObjectItemCaseSensitive(command, "a");
  cJSON* body = cJSON_GetObjectItemCaseSensitive(command, "b");
  cJSON* path = cJSON_GetObjectItemCaseSensitive(body, "p");
//  cJSON* hash = cJSON_GetObjectItemCaseSensitive(body, "h");

  if ((!request_id || cJSON_IsNumber(request_id)) &&
      cJSON_IsString(action) && action->valuestring != nullptr &&
      cJSON_IsObject(body)) {
    // TODO(awong): Match the request_id? Do we even care to track?
    // We can do skipped messages I guess.
    // There are 2 action types received:
    //   d - a JSON tree is being replaced.
    //   m - a JSON tree should be merged. [ TODO(awong): what does this mean? Don't delete? ]
    unique_cJSON_ptr new_data(cJSON_DetachItemFromObjectCaseSensitive(body, "d"));
    if (strcmp(action->valuestring, "d") == 0) {
      ReplacePath(path->valuestring, std::move(new_data));
    } if (strcmp(action->valuestring, "m") == 0) {
      MergePath(path->valuestring, std::move(new_data));
    }
  }
}

void FirebaseDatabase::ReplacePath(const char* path, unique_cJSON_ptr new_data) {
  cJSON* parent;
  cJSON* node;
  std::string key;
  GetPath(path, &parent, &node, true, &key);
  if (parent) {
    cJSON_ReplaceItemInObjectCaseSensitive(parent, key.c_str(),  new_data.release());
  } else {
    root_ = std::move(new_data);
    // TODO(awong): Fix cJSON To correct item->string which isn't erased here after detach.
  }

  // Firebase doesn't support nulls. This garbage collection step keeps us consistent.
  RemoveEmptyNodes(root_.get());
}

void FirebaseDatabase::MergePath(const char* path, unique_cJSON_ptr new_data) {
  // new_data is actually a key/value pair of relative _paths_ from the root.
  // Each path is considered an overwrite operation.
  cJSON* update = new_data->child;
  while (update) {
    // Cache next pointer as item will be detached in loop.
    cJSON* next = update->next;
    std::string update_path = path;
    update_path += "/";
    update_path += update->string;
    unique_cJSON_ptr update_node(cJSON_DetachItemViaPointer(new_data.get(), update));
    ReplacePath(update_path.c_str(), std::move(update_node));
    update = next;
  }
}

void FirebaseDatabase::SendKeepalive() {
  static constexpr int kKeepAliveMs = 45000;
  websocket_.SendText("0");
  event_manager_->RunDelayed([&] {SendKeepalive();}, kKeepAliveMs);
}

}  // namespace esp_cxx
