// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/user_image_sync_observer.h"

#include "base/bind.h"
#include "base/prefs/pref_change_registrar.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/chromeos/login/default_user_images.h"
#include "chrome/browser/chromeos/login/screens/user_image_screen.h"
#include "chrome/browser/chromeos/login/user.h"
#include "chrome/browser/chromeos/login/user_image_manager.h"
#include "chrome/browser/chromeos/login/wizard_controller.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/prefs/pref_service_syncable.h"
#include "chrome/browser/prefs/scoped_user_pref_update.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_service.h"

namespace chromeos {
namespace {

// A dictionary containing info about user image.
const char kUserImageInfo[] = "user_image_info";
// Path to value with image index.
const char kImageIndex[] = "image_index";

bool IsIndexSupported(int index) {
  return (index >= kFirstDefaultImageIndex && index < kDefaultImagesCount) ||
      (index == User::kProfileImageIndex);
}

Profile* GetUserProfile() {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager)
    return NULL;
  base::FilePath profile_path = profile_manager->GetInitialProfileDir();
  return profile_manager->GetProfileByPath(profile_path);
}

}  // anonymous namespace

UserImageSyncObserver::Observer::~Observer() {}

UserImageSyncObserver::UserImageSyncObserver(const User* user)
    : user_(user),
      prefs_(NULL),
      is_synced_(false),
      local_image_changed_(false) {
  notification_registrar_.reset(new content::NotificationRegistrar);
  notification_registrar_->Add(this,
      chrome::NOTIFICATION_LOGIN_USER_IMAGE_CHANGED,
      content::NotificationService::AllSources());
  Profile* profile = GetUserProfile();
  if (profile) {
    OnProfileGained(profile);
  } else {
    notification_registrar_->Add(this,
        chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED,
        content::NotificationService::AllSources());
  }
}

UserImageSyncObserver::~UserImageSyncObserver() {
  if (!is_synced_ && prefs_)
    prefs_->RemoveObserver(this);
  if (pref_change_registrar_)
    pref_change_registrar_->RemoveAll();
}

// static
void UserImageSyncObserver::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry_) {
  registry_->RegisterDictionaryPref(
      kUserImageInfo,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PRIORITY_PREF);
}

void UserImageSyncObserver::AddObserver(Observer* observer) {
  observer_list_.AddObserver(observer);
}

void UserImageSyncObserver::RemoveObserver(Observer* observer) {
  observer_list_.RemoveObserver(observer);
}

void UserImageSyncObserver::OnProfileGained(Profile* profile) {
  prefs_ = PrefServiceSyncable::FromProfile(profile);
  pref_change_registrar_.reset(new PrefChangeRegistrar);
  pref_change_registrar_->Init(prefs_);
  pref_change_registrar_->Add(kUserImageInfo,
      base::Bind(&UserImageSyncObserver::OnPreferenceChanged,
                 base::Unretained(this)));
  is_synced_ = prefs_->IsPrioritySyncing();
  if (!is_synced_) {
    prefs_->AddObserver(this);
  } else {
    OnInitialSync();
  }
}

void UserImageSyncObserver::OnInitialSync() {
  int synced_index;
  bool local_image_updated = false;
  if (!GetSyncedImageIndex(&synced_index) || local_image_changed_) {
    UpdateSyncedImageFromLocal();
  } else if (IsIndexSupported(synced_index) && CanUpdateLocalImageNow()) {
    UpdateLocalImageFromSynced();
    local_image_updated = true;
  }
  FOR_EACH_OBSERVER(UserImageSyncObserver::Observer, observer_list_,
      OnInitialSync(local_image_updated));
}

void UserImageSyncObserver::OnPreferenceChanged(const std::string& pref_name) {
  // OnPreferenceChanged can be called before OnIsSyncingChanged.
  if (!is_synced_) {
    is_synced_ = true;
    prefs_->RemoveObserver(this);
    OnInitialSync();
  } else if (CanUpdateLocalImageNow()) {
    UpdateLocalImageFromSynced();
  }
}

void UserImageSyncObserver::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  if (type == chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED) {
    Profile* profile = content::Details<Profile>(details).ptr();
    if (GetUserProfile() != profile)
      return;
    notification_registrar_->Remove(
        this,
        chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED,
        content::NotificationService::AllSources());
    OnProfileGained(profile);
  } else if (type == chrome::NOTIFICATION_LOGIN_USER_IMAGE_CHANGED) {
    if (is_synced_)
      UpdateSyncedImageFromLocal();
    else
      local_image_changed_ = true;
  } else {
    NOTREACHED();
  }
}

void UserImageSyncObserver::OnIsSyncingChanged() {
  is_synced_ = prefs_->IsPrioritySyncing();
  if (is_synced_) {
    prefs_->RemoveObserver(this);
    OnInitialSync();
  }
}

void UserImageSyncObserver::UpdateSyncedImageFromLocal() {
  int local_index = user_->image_index();
  if (!IsIndexSupported(local_index)) {
    local_index = User::kInvalidImageIndex;
  }
  int synced_index;
  if (GetSyncedImageIndex(&synced_index) && (synced_index == local_index))
    return;
  DictionaryPrefUpdate update(prefs_, kUserImageInfo);
  DictionaryValue* dict = update.Get();
  dict->SetInteger(kImageIndex, local_index);
  LOG(INFO) << "Saved avatar index " << local_index << " to sync.";
}

void UserImageSyncObserver::UpdateLocalImageFromSynced() {
  int synced_index;
  GetSyncedImageIndex(&synced_index);
  int local_index = user_->image_index();
  if ((synced_index == local_index) || !IsIndexSupported(synced_index))
    return;
  UserImageManager* image_manager = UserManager::Get()->GetUserImageManager();
  if (synced_index == User::kProfileImageIndex) {
    image_manager->SaveUserImageFromProfileImage(user_->email());
  } else {
    image_manager->SaveUserDefaultImageIndex(user_->email(), synced_index);
  }
  LOG(INFO) << "Loaded avatar index " << synced_index << " from sync.";
}

bool UserImageSyncObserver::GetSyncedImageIndex(int* index) {
  *index = User::kInvalidImageIndex;
  const DictionaryValue* dict = prefs_->GetDictionary(kUserImageInfo);
  return dict && dict->GetInteger(kImageIndex, index);
}

bool UserImageSyncObserver::CanUpdateLocalImageNow() {
  if (WizardController* wizard_controller =
          WizardController::default_controller()) {
    UserImageScreen* screen = wizard_controller->GetUserImageScreen();
    if (wizard_controller->current_screen() == screen) {
      if (screen->user_selected_image())
        return false;
    }
  }
  return true;
}

}  // namespace chromeos

