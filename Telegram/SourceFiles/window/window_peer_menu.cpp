/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "window/window_peer_menu.h"

#include "lang/lang_keys.h"
#include "boxes/confirm_box.h"
#include "boxes/mute_settings_box.h"
#include "boxes/add_contact_box.h"
#include "boxes/report_box.h"
#include "boxes/peer_list_controllers.h"
#include "boxes/peers/manage_peer_box.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "mainwidget.h"
#include "observer_peer.h"
#include "styles/style_boxes.h"
#include "window/window_controller.h"

namespace Window {
namespace {

void AddChatMembers(not_null<ChatData*> chat) {
	if (chat->count >= Global::ChatSizeMax() && chat->amCreator()) {
		Ui::show(Box<ConvertToSupergroupBox>(chat));
	} else {
		AddParticipantsBoxController::Start(chat);
	}
}

void AddChannelMembers(not_null<ChannelData*> channel) {
	if (channel->isMegagroup()) {
		auto &participants = channel->mgInfo->lastParticipants;
		AddParticipantsBoxController::Start(
			channel,
			{ participants.cbegin(), participants.cend() });
	} else if (channel->membersCount() >= Global::ChatSizeMax()) {
		Ui::show(
			Box<MaxInviteBox>(channel),
			LayerOption::KeepOther);
	} else {
		AddParticipantsBoxController::Start(channel, { });
	}
}

class Filler {
public:
	Filler(
		not_null<Controller*> controller,
		not_null<PeerData*> peer,
		const PeerMenuCallback &addAction,
		PeerMenuSource source);
	void fill();

private:
	bool showInfo();
	void addPinToggle();
	void addInfo();
	void addNotifications();
	void addSearch();
	void addUserActions(not_null<UserData*> user);
	void addBlockUser(not_null<UserData*> user);
	void addChatActions(not_null<ChatData*> chat);
	void addChannelActions(not_null<ChannelData*> channel);

	not_null<Controller*> _controller;
	not_null<PeerData*> _peer;
	const PeerMenuCallback &_addAction;
	PeerMenuSource _source;

};

History *FindWastedPin() {
	auto order = App::histories().getPinnedOrder();
	for_const (auto pinned, order) {
		if (pinned->peer->isChat()
			&& pinned->peer->asChat()->isDeactivated()
			&& !pinned->inChatList(Dialogs::Mode::All)) {
			return pinned;
		}
	}
	return nullptr;
}

auto ClearHistoryHandler(not_null<PeerData*> peer) {
	return [peer] {
		auto text = peer->isUser() ? lng_sure_delete_history(lt_contact, peer->name) : lng_sure_delete_group_history(lt_group, peer->name);
		Ui::show(Box<ConfirmBox>(text, lang(lng_box_delete), st::attentionBoxButton, [peer] {
			if (!App::main()) return;

			Ui::hideLayer();
			App::main()->clearHistory(peer);
		}));
	};
}

auto DeleteAndLeaveHandler(not_null<PeerData*> peer) {
	return [peer] {
		auto warningText = peer->isUser() ? lng_sure_delete_history(lt_contact, peer->name) :
			peer->isChat() ? lng_sure_delete_and_exit(lt_group, peer->name) :
			lang(peer->isMegagroup() ? lng_sure_leave_group : lng_sure_leave_channel);
		auto confirmText = lang(peer->isUser() ? lng_box_delete : lng_box_leave);
		auto &confirmStyle = peer->isChannel() ? st::defaultBoxButton : st::attentionBoxButton;
		Ui::show(Box<ConfirmBox>(warningText, confirmText, confirmStyle, [peer] {
			if (!App::main()) return;

			Ui::hideLayer();
			Ui::showChatsList();
			if (peer->isUser()) {
				App::main()->deleteConversation(peer);
			} else if (auto chat = peer->asChat()) {
				App::main()->deleteAndExit(chat);
			} else if (auto channel = peer->asChannel()) {
				// Don't delete old history by default,
				// because Android app doesn't.
				//
				//if (auto migrateFrom = channel->migrateFrom()) {
				//	App::main()->deleteConversation(migrateFrom);
				//}
				Auth().api().leaveChannel(channel);
			}
		}));
	};
}

Filler::Filler(
	not_null<Controller*> controller,
	not_null<PeerData*> peer,
	const PeerMenuCallback &addAction,
	PeerMenuSource source)
: _controller(controller)
, _peer(peer)
, _addAction(addAction)
, _source(source) {
}

bool Filler::showInfo() {
	if (_source == PeerMenuSource::Profile) {
		return false;
	} else if (_controller->activePeer.current() != _peer) {
		return true;
	} else if (!Adaptive::ThreeColumn()) {
		return true;
	} else if (
		!Auth().data().thirdSectionInfoEnabled() &&
		!Auth().data().tabbedReplacedWithInfo()) {
		return true;
	}
	return false;
}

void Filler::addPinToggle() {
	auto peer = _peer;
	auto isPinned = false;
	if (auto history = App::historyLoaded(peer)) {
		isPinned = history->isPinnedDialog();
	}
	auto pinText = [](bool isPinned) {
		return lang(isPinned
			? lng_context_unpin_from_top
			: lng_context_pin_to_top);
	};
	auto pinToggle = [peer] {
		auto history = App::history(peer);
		auto isPinned = !history->isPinnedDialog();
		auto pinnedCount = App::histories().pinnedCount();
		auto pinnedMax = Global::PinnedDialogsCountMax();
		if (isPinned && pinnedCount >= pinnedMax) {
			// Some old chat, that was converted to supergroup, maybe is still pinned.
			if (auto wasted = FindWastedPin()) {
				wasted->setPinnedDialog(false);
				history->setPinnedDialog(isPinned);
				App::histories().savePinnedToServer();
			} else {
				auto errorText = lng_error_pinned_max(
					lt_count,
					pinnedMax);
				Ui::show(Box<InformBox>(errorText));
			}
			return;
		}

		history->setPinnedDialog(isPinned);
		auto flags = MTPmessages_ToggleDialogPin::Flags(0);
		if (isPinned) {
			flags |= MTPmessages_ToggleDialogPin::Flag::f_pinned;
		}
		MTP::send(MTPmessages_ToggleDialogPin(MTP_flags(flags), peer->input));
		if (isPinned) {
			if (auto main = App::main()) {
				main->dialogsToUp();
			}
		}
	};
	auto pinAction = _addAction(pinText(isPinned), pinToggle);

	auto lifetime = Notify::PeerUpdateViewer(
		peer,
		Notify::PeerUpdate::Flag::PinnedChanged)
		| rpl::start_with_next([peer, pinAction, pinText] {
			auto isPinned = App::history(peer)->isPinnedDialog();
			pinAction->setText(pinText(isPinned));
		});

	Ui::AttachAsChild(pinAction, std::move(lifetime));
}

void Filler::addInfo() {
	auto controller = _controller;
	auto peer = _peer;
	auto infoKey = (peer->isChat() || peer->isMegagroup())
		? lng_context_view_group
		: (peer->isUser()
			? lng_context_view_profile
			: lng_context_view_channel);
	_addAction(lang(infoKey), [=] {
		controller->showPeerInfo(peer);
	});
}

void Filler::addNotifications() {
	auto peer = _peer;
	auto muteText = [](bool isMuted) {
		return lang(isMuted
			? lng_enable_notifications_from_tray
			: lng_disable_notifications_from_tray);
	};
	auto muteAction = _addAction(muteText(peer->isMuted()), [peer] {
		if (!peer->isMuted()) {
			Ui::show(Box<MuteSettingsBox>(peer));
		} else {
			App::main()->updateNotifySetting(
				peer,
				NotifySettingSetNotify);
		}
	});

	auto lifetime = Notify::PeerUpdateViewer(
		_peer,
		Notify::PeerUpdate::Flag::NotificationsEnabled)
		| rpl::start_with_next([=] {
			muteAction->setText(muteText(peer->isMuted()));
		});

	Ui::AttachAsChild(muteAction, std::move(lifetime));
}

void Filler::addSearch() {
	_addAction(lang(lng_profile_search_messages), [peer = _peer] {
		App::main()->searchInPeer(peer);
	});
}

void Filler::addBlockUser(not_null<UserData*> user) {
	auto blockText = [](not_null<UserData*> user) {
		return lang(user->isBlocked()
			? (user->botInfo
				? lng_profile_unblock_bot
				: lng_profile_unblock_user)
			: (user->botInfo
				? lng_profile_block_bot
				: lng_profile_block_user));
	};
	auto blockAction = _addAction(blockText(user), [user] {
		auto willBeBlocked = !user->isBlocked();
		auto handler = ::rpcDone([user, willBeBlocked](const MTPBool &result) {
			user->setBlockStatus(willBeBlocked
				? UserData::BlockStatus::Blocked
				: UserData::BlockStatus::NotBlocked);
		});
		if (willBeBlocked) {
			MTP::send(
				MTPcontacts_Block(user->inputUser),
				std::move(handler));
		} else {
			MTP::send(
				MTPcontacts_Unblock(user->inputUser),
				std::move(handler));
		}
	});

	auto lifetime = Notify::PeerUpdateViewer(
		_peer,
		Notify::PeerUpdate::Flag::UserIsBlocked)
		| rpl::start_with_next([=] {
			blockAction->setText(blockText(user));
		});

	Ui::AttachAsChild(blockAction, std::move(lifetime));

	if (user->blockStatus() == UserData::BlockStatus::Unknown) {
		Auth().api().requestFullPeer(user);
	}
}

void Filler::addUserActions(not_null<UserData*> user) {
	if (_source != PeerMenuSource::ChatsList) {
		if (user->isContact()) {
			_addAction(
				lang(lng_info_share_contact),
				[user] { PeerMenuShareContactBox(user); });
			_addAction(
				lang(lng_info_edit_contact),
				[user] { Ui::show(Box<AddContactBox>(user)); });
			_addAction(
				lang(lng_info_delete_contact),
				[user] { PeerMenuDeleteContact(user); });
		} else if (user->canShareThisContact()) {
			_addAction(
				lang(lng_info_add_as_contact),
				[user] { PeerMenuAddContact(user); });
			_addAction(
				lang(lng_info_share_contact),
				[user] { PeerMenuShareContactBox(user); });
		} else if (user->botInfo && !user->botInfo->cantJoinGroups) {
			_addAction(
				lang(lng_profile_invite_to_group),
				[user] { AddBotToGroupBoxController::Start(user); });
		}
	}
	_addAction(
		lang(lng_profile_delete_conversation),
		DeleteAndLeaveHandler(user));
	_addAction(
		lang(lng_profile_clear_history),
		ClearHistoryHandler(user));
	if (!user->isInaccessible() && user != App::self()) {
		addBlockUser(user);
	}
}

void Filler::addChatActions(not_null<ChatData*> chat) {
	if (_source != PeerMenuSource::ChatsList) {
		if (chat->canEdit()) {
			_addAction(
				lang(lng_profile_edit_contact),
				[chat] { Ui::show(Box<EditNameTitleBox>(chat)); });
		}
		if (chat->amCreator()
			&& !chat->isDeactivated()) {
			_addAction(
				lang(lng_profile_manage_admins),
				[chat] { EditChatAdminsBoxController::Start(chat); });
			_addAction(
				lang(lng_profile_migrate_button),
				[chat] { Ui::show(Box<ConvertToSupergroupBox>(chat)); });
		}
		if (chat->canEdit()) {
			_addAction(
				lang(lng_profile_add_participant),
				[chat] { AddChatMembers(chat); });
		}
	}
	_addAction(
		lang(lng_profile_clear_and_exit),
		DeleteAndLeaveHandler(_peer));
	_addAction(
		lang(lng_profile_clear_history),
		ClearHistoryHandler(_peer));
}

void Filler::addChannelActions(not_null<ChannelData*> channel) {
	if (_source != PeerMenuSource::ChatsList) {
		if (ManagePeerBox::Available(channel)) {
			auto text = lang(channel->isMegagroup()
				? lng_manage_group_title
				: lng_manage_channel_title);
			_addAction(text, [channel] {
				Ui::show(Box<ManagePeerBox>(channel));
			});
		}
		if (channel->canAddMembers()) {
			_addAction(
				lang(lng_channel_add_members),
				[channel] { AddChannelMembers(channel); });
		}
	}
	if (channel->amIn()) {
		auto leaveText = lang(channel->isMegagroup()
			? lng_profile_leave_group
			: lng_profile_leave_channel);
		_addAction(leaveText, DeleteAndLeaveHandler(channel));
	} else {
		auto joinText = lang(channel->isMegagroup()
			? lng_profile_join_group
			: lng_profile_join_channel);
		_addAction(
			joinText,
			[channel] { Auth().api().joinChannel(channel); });
	}
	if (_source != PeerMenuSource::ChatsList) {
		auto needReport = !channel->amCreator()
			&& (!channel->isMegagroup() || channel->isPublic());
		if (needReport) {
			_addAction(lang(lng_profile_report), [channel] {
				Ui::show(Box<ReportBox>(channel));
			});
		}
	}
}

void Filler::fill() {
	if (_source == PeerMenuSource::ChatsList) {
		addPinToggle();
	}
	if (showInfo()) {
		addInfo();
	}
	if (_source != PeerMenuSource::Profile) {
		addNotifications();
	}
	if (_source == PeerMenuSource::ChatsList) {
		addSearch();
	}

	if (auto user = _peer->asUser()) {
		addUserActions(user);
	} else if (auto chat = _peer->asChat()) {
		addChatActions(chat);
	} else if (auto channel = _peer->asChannel()) {
		addChannelActions(channel);
	}
}

} // namespace

void PeerMenuDeleteContact(not_null<UserData*> user) {
	auto text = lng_sure_delete_contact(
		lt_contact,
		App::peerName(user));
	auto deleteSure = [=] {
		Ui::showChatsList();
		Ui::hideLayer();
		MTP::send(
			MTPcontacts_DeleteContact(user->inputUser),
			App::main()->rpcDone(
				&MainWidget::deletedContact,
				user.get()));
	};
	auto box = Box<ConfirmBox>(
		text,
		lang(lng_box_delete),
		std::move(deleteSure));
	Ui::show(std::move(box));
}

void PeerMenuAddContact(not_null<UserData*> user) {
	auto firstName = user->firstName;
	auto lastName = user->lastName;
	auto phone = user->phone().isEmpty()
		? App::phoneFromSharedContact(user->bareId())
		: user->phone();
	Ui::show(Box<AddContactBox>(firstName, lastName, phone));
}

void PeerMenuShareContactBox(not_null<UserData*> user) {
	auto callback = [user](not_null<PeerData*> peer) {
		if (!peer->canWrite()) {
			Ui::show(Box<InformBox>(
				lang(lng_forward_share_cant)),
				LayerOption::KeepOther);
			return;
		}
		auto recipient = peer->isUser()
			? peer->name
			: '\xAB' + peer->name + '\xBB';
		Ui::show(Box<ConfirmBox>(
			lng_forward_share_contact(lt_recipient, recipient),
			lang(lng_forward_send),
			[peer, user] {
				App::main()->onShareContact(
					peer->id,
					user);
				Ui::hideLayer();
			}), LayerOption::KeepOther);
	};
	Ui::show(Box<PeerListBox>(
		std::make_unique<ChooseRecipientBoxController>(std::move(callback)),
		[](not_null<PeerListBox*> box) {
			box->addButton(langFactory(lng_cancel), [box] {
				box->closeBox();
			});
		}));
}

void FillPeerMenu(
		not_null<Controller*> controller,
		not_null<PeerData*> peer,
		const PeerMenuCallback &callback,
		PeerMenuSource source) {
	Filler filler(controller, peer, callback, source);
	filler.fill();
}

} // namespace Window