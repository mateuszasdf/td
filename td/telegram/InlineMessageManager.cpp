//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/InlineMessageManager.h"

#include "td/telegram/AuthManager.h"
#include "td/telegram/DialogManager.h"
#include "td/telegram/files/FileManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/InlineQueriesManager.h"
#include "td/telegram/InputMessageText.h"
#include "td/telegram/Location.h"
#include "td/telegram/MessageContent.h"
#include "td/telegram/MessageEntity.h"
#include "td/telegram/net/DcId.h"
#include "td/telegram/OptionManager.h"
#include "td/telegram/ReplyMarkup.h"
#include "td/telegram/Td.h"

#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/Status.h"

namespace td {

class EditInlineMessageQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit EditInlineMessageQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(telegram_api::object_ptr<telegram_api::InputBotInlineMessageID> input_bot_inline_message_id,
            bool force_edit_text, const string &text,
            vector<telegram_api::object_ptr<telegram_api::MessageEntity>> &&entities, bool disable_web_page_preview,
            telegram_api::object_ptr<telegram_api::InputMedia> &&input_media, bool invert_media,
            telegram_api::object_ptr<telegram_api::ReplyMarkup> &&reply_markup) {
    CHECK(input_bot_inline_message_id != nullptr);

    // file in an inline message can't be uploaded to another datacenter,
    // so only previously uploaded files or URLs can be used in the InputMedia
    CHECK(!FileManager::extract_was_uploaded(input_media));

    int32 flags = 0;
    if (disable_web_page_preview) {
      flags |= telegram_api::messages_editInlineBotMessage::NO_WEBPAGE_MASK;
    }
    if (reply_markup != nullptr) {
      flags |= telegram_api::messages_editInlineBotMessage::REPLY_MARKUP_MASK;
    }
    if (!entities.empty()) {
      flags |= telegram_api::messages_editInlineBotMessage::ENTITIES_MASK;
    }
    if (force_edit_text || !text.empty()) {
      flags |= telegram_api::messages_editInlineBotMessage::MESSAGE_MASK;
    }
    if (input_media != nullptr) {
      flags |= telegram_api::messages_editInlineBotMessage::MEDIA_MASK;
    }
    if (invert_media) {
      flags |= telegram_api::messages_editInlineBotMessage::INVERT_MEDIA_MASK;
    }

    auto dc_id = DcId::internal(InlineQueriesManager::get_inline_message_dc_id(input_bot_inline_message_id));
    send_query(G()->net_query_creator().create(
        telegram_api::messages_editInlineBotMessage(
            flags, false /*ignored*/, false /*ignored*/, std::move(input_bot_inline_message_id), text,
            std::move(input_media), std::move(reply_markup), std::move(entities)),
        {}, dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_editInlineBotMessage>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    LOG_IF(ERROR, !result_ptr.ok()) << "Receive false in result of editInlineMessage";

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for EditInlineMessageQuery: " << status;
    promise_.set_error(std::move(status));
  }
};

InlineMessageManager::InlineMessageManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

void InlineMessageManager::tear_down() {
  parent_.reset();
}

void InlineMessageManager::edit_inline_message_text(
    const string &inline_message_id, td_api::object_ptr<td_api::ReplyMarkup> &&reply_markup,
    td_api::object_ptr<td_api::InputMessageContent> &&input_message_content, Promise<Unit> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  if (input_message_content == nullptr) {
    return promise.set_error(Status::Error(400, "Can't edit message without new content"));
  }
  int32 new_message_content_type = input_message_content->get_id();
  if (new_message_content_type != td_api::inputMessageText::ID) {
    return promise.set_error(Status::Error(400, "Input message content type must be InputMessageText"));
  }

  TRY_RESULT_PROMISE(
      promise, input_message_text,
      process_input_message_text(td_, DialogId(), std::move(input_message_content), td_->auth_manager_->is_bot()));
  TRY_RESULT_PROMISE(promise, new_reply_markup,
                     get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true));

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), true, input_message_text.text.text,
             get_input_message_entities(td_->user_manager_.get(), input_message_text.text.entities,
                                        "edit_inline_message_text"),
             input_message_text.disable_web_page_preview, input_message_text.get_input_media_web_page(),
             input_message_text.show_above_text, get_input_reply_markup(td_->user_manager_.get(), new_reply_markup));
}

void InlineMessageManager::edit_inline_message_live_location(const string &inline_message_id,
                                                             td_api::object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                             td_api::object_ptr<td_api::location> &&input_location,
                                                             int32 live_period, int32 heading,
                                                             int32 proximity_alert_radius, Promise<Unit> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  TRY_RESULT_PROMISE(promise, new_reply_markup,
                     get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true));

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  Location location(input_location);
  if (location.empty() && input_location != nullptr) {
    return promise.set_error(Status::Error(400, "Invalid location specified"));
  }

  int32 flags = 0;
  if (location.empty()) {
    flags |= telegram_api::inputMediaGeoLive::STOPPED_MASK;
  }
  if (live_period != 0) {
    flags |= telegram_api::inputMediaGeoLive::PERIOD_MASK;
  }
  if (heading != 0) {
    flags |= telegram_api::inputMediaGeoLive::HEADING_MASK;
  }
  flags |= telegram_api::inputMediaGeoLive::PROXIMITY_NOTIFICATION_RADIUS_MASK;
  auto input_media = telegram_api::make_object<telegram_api::inputMediaGeoLive>(
      flags, false /*ignored*/, location.get_input_geo_point(), heading, live_period, proximity_alert_radius);
  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), false, string(),
             vector<telegram_api::object_ptr<telegram_api::MessageEntity>>(), false, std::move(input_media),
             false /*ignored*/, get_input_reply_markup(td_->user_manager_.get(), new_reply_markup));
}

void InlineMessageManager::edit_inline_message_media(
    const string &inline_message_id, td_api::object_ptr<td_api::ReplyMarkup> &&reply_markup,
    td_api::object_ptr<td_api::InputMessageContent> &&input_message_content, Promise<Unit> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  if (input_message_content == nullptr) {
    return promise.set_error(Status::Error(400, "Can't edit message without new content"));
  }
  int32 new_message_content_type = input_message_content->get_id();
  if (new_message_content_type != td_api::inputMessageAnimation::ID &&
      new_message_content_type != td_api::inputMessageAudio::ID &&
      new_message_content_type != td_api::inputMessageDocument::ID &&
      new_message_content_type != td_api::inputMessagePhoto::ID &&
      new_message_content_type != td_api::inputMessageVideo::ID) {
    return promise.set_error(Status::Error(400, "Unsupported input message content type"));
  }

  bool is_premium = td_->option_manager_->get_option_boolean("is_premium");
  TRY_RESULT_PROMISE(promise, content,
                     get_input_message_content(DialogId(), std::move(input_message_content), td_, is_premium));
  if (!content.ttl.is_empty()) {
    return promise.set_error(Status::Error(400, "Can't enable self-destruction for media"));
  }

  TRY_RESULT_PROMISE(promise, new_reply_markup,
                     get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true));

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  auto input_media = get_input_media(content.content.get(), td_, MessageSelfDestructType(), string(), true);
  if (input_media == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid message content specified"));
  }

  const FormattedText *caption = get_message_content_caption(content.content.get());
  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), true, caption == nullptr ? "" : caption->text,
             get_input_message_entities(td_->user_manager_.get(), caption, "edit_inline_message_media"), false,
             std::move(input_media), content.invert_media,
             get_input_reply_markup(td_->user_manager_.get(), new_reply_markup));
}

void InlineMessageManager::edit_inline_message_caption(const string &inline_message_id,
                                                       td_api::object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                       td_api::object_ptr<td_api::formattedText> &&input_caption,
                                                       bool invert_media, Promise<Unit> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  TRY_RESULT_PROMISE(promise, caption,
                     get_formatted_text(td_, td_->dialog_manager_->get_my_dialog_id(), std::move(input_caption),
                                        td_->auth_manager_->is_bot(), true, false, false));
  TRY_RESULT_PROMISE(promise, new_reply_markup,
                     get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true));

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), true, caption.text,
             get_input_message_entities(td_->user_manager_.get(), caption.entities, "edit_inline_message_caption"),
             false, nullptr, invert_media, get_input_reply_markup(td_->user_manager_.get(), new_reply_markup));
}

void InlineMessageManager::edit_inline_message_reply_markup(const string &inline_message_id,
                                                            td_api::object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                            Promise<Unit> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  TRY_RESULT_PROMISE(promise, new_reply_markup,
                     get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true));

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), false, string(),
             vector<telegram_api::object_ptr<telegram_api::MessageEntity>>(), false, nullptr, false /*ignored*/,
             get_input_reply_markup(td_->user_manager_.get(), new_reply_markup));
}

}  // namespace td
