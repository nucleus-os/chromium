// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy/chrome_browser_policy_connector.h"

#include <memory>
#include <string>
#include <utility>

#include "base/check_is_test.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "cef/libcef/features/features.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/enterprise/browser_management/management_service_factory.h"
#include "chrome/browser/policy/configuration_policy_handler_list_factory.h"
#include "chrome/browser/policy/device_management_service_configuration.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/chrome_paths.h"
#include "chrome_browser_policy_connector.h"
#include "components/policy/core/common/async_policy_provider.h"
#include "components/policy/core/common/cloud/cloud_external_data_manager.h"
#include "components/policy/core/common/cloud/cloud_policy_client_registration_helper.h"
#include "components/policy/core/common/cloud/device_management_service.h"
#include "components/policy/core/common/cloud/user_cloud_policy_manager.h"
#include "components/policy/core/common/command_line_policy_provider.h"
#include "components/policy/core/common/configuration_policy_provider.h"
#include "components/policy/core/common/local_test_policy_provider.h"
#include "components/policy/core/common/policy_logger.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/core/common/policy_namespace.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "components/policy/core/common/policy_proto_decoders.h"
#include "components/policy/core/common/policy_types.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_service.h"
#include "content/public/common/content_switches.h"
#include "extensions/buildflags/buildflags.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/registry.h"
#include "chrome/browser/browser_switcher/browser_switcher_policy_migrator.h"
#include "components/policy/core/common/policy_loader_win.h"
#elif BUILDFLAG(IS_MAC)
#include <CoreFoundation/CoreFoundation.h>

#include "base/apple/foundation_util.h"
#include "base/strings/sys_string_conversions.h"
#include "components/policy/core/common/policy_loader_mac.h"
#include "components/policy/core/common/preferences_mac.h"
#elif BUILDFLAG(IS_ANDROID)
#include "chrome/browser/policy/chrome_browser_cloud_management_controller_android.h"
#include "components/policy/core/common/android/android_combined_policy_provider.h"
#elif BUILDFLAG(IS_POSIX)
#include "components/policy/core/common/config_dir_policy_loader.h"
#endif

#if !BUILDFLAG(IS_CHROMEOS)
#include "chrome/browser/policy/chrome_browser_cloud_management_controller_desktop.h"
#include "components/enterprise/browser/controller/chrome_browser_cloud_management_controller.h"
#include "components/policy/core/common/cloud/machine_level_user_cloud_policy_manager.h"
#include "components/policy/core/common/proxy_policy_provider.h"
#endif

namespace policy {
namespace {
bool g_command_line_enabled_for_testing = false;

std::string* PlatformPolicyId() {
  static base::NoDestructor<std::string> id;
  return id.get();
}
}  // namespace

ChromeBrowserPolicyConnector::ChromeBrowserPolicyConnector()
    : BrowserPolicyConnector(base::BindRepeating(&BuildHandlerList)) {
#if !BUILDFLAG(IS_CHROMEOS)
  std::unique_ptr<ChromeBrowserCloudManagementController::Delegate> delegate =
#if BUILDFLAG(IS_ANDROID)
      std::make_unique<ChromeBrowserCloudManagementControllerAndroid>();
#else
      std::make_unique<ChromeBrowserCloudManagementControllerDesktop>();
#endif

  chrome_browser_cloud_management_controller_ =
      std::make_unique<ChromeBrowserCloudManagementController>(
          std::move(delegate));
#endif
}

ChromeBrowserPolicyConnector::~ChromeBrowserPolicyConnector() {
  if (local_test_provider_) {
    local_test_provider_->Shutdown();
  }
}

void ChromeBrowserPolicyConnector::OnResourceBundleCreated() {
  BrowserPolicyConnectorBase::OnResourceBundleCreated();
}

void ChromeBrowserPolicyConnector::Init(
    PrefService* local_state,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  PolicyLogger::GetInstance()->EnableLogDeletion();
  auto configuration = std::make_unique<DeviceManagementServiceConfiguration>(
      GetDeviceManagementUrl(), GetRealtimeReportingUrl(),
      GetEncryptedReportingUrl());
  auto device_management_service =
      std::make_unique<DeviceManagementService>(std::move(configuration));
  device_management_service->ScheduleInitialization(
      kServiceInitializationStartupDelay);

#if BUILDFLAG(IS_ANDROID)
  policy_cache_updater_ = std::make_unique<android::PolicyCacheUpdater>(
      GetPolicyService(), GetHandlerList());
#endif

  InitInternal(local_state, std::move(device_management_service));
}

void ChromeBrowserPolicyConnector::OnBrowserStarted() {}

bool ChromeBrowserPolicyConnector::IsDeviceEnterpriseManaged() const {
  NOTREACHED() << "This method is only defined for ChromeOS";
}

bool ChromeBrowserPolicyConnector::HasMachineLevelPolicies() {
  if (ProviderHasPolicies(GetPlatformProvider())) {
    return true;
  }
#if !BUILDFLAG(IS_CHROMEOS)
  if (ProviderHasPolicies(machine_level_user_cloud_policy_manager())) {
    return true;
  }
#endif  // !BUILDFLAG(IS_CHROMEOS)
  if (ProviderHasPolicies(command_line_provider_)) {
    return true;
  }
  return false;
}

void ChromeBrowserPolicyConnector::Shutdown() {
#if !BUILDFLAG(IS_CHROMEOS)
  // Reset the controller before calling base class so that
  // shutdown occurs in correct sequence.
  chrome_browser_cloud_management_controller_.reset();

  if (machine_level_user_cloud_policy_manager_) {
    machine_level_user_cloud_policy_manager_->Shutdown();
    machine_level_user_cloud_policy_manager_ = nullptr;
  }

  if (HasPolicyService()) {
    GetPolicyService()->UseLocalTestPolicyProvider(nullptr);
  }
#endif

  BrowserPolicyConnector::Shutdown();
}

ConfigurationPolicyProvider*
ChromeBrowserPolicyConnector::GetPlatformProvider() {
  if (ConfigurationPolicyProvider* provider =
          BrowserPolicyConnectorBase::GetPolicyProviderForTesting()) {
    CHECK_IS_TEST();
    return provider;
  }
  return platform_provider_.get();
}

void ChromeBrowserPolicyConnector::RefreshPlatformPolicies() {
  if (ConfigurationPolicyProvider* platform_provider = GetPlatformProvider()) {
    platform_provider->RefreshPolicies(policy::PolicyFetchReason::kUserRequest);
  }
}

ConfigurationPolicyProvider*
ChromeBrowserPolicyConnector::local_test_policy_provider() {
  if (local_test_provider_for_testing_) {
    return local_test_provider_for_testing_.get();
  }
  return local_test_provider_.get();
}

void ChromeBrowserPolicyConnector::SetLocalTestPolicyProviderForTesting(
    ConfigurationPolicyProvider* provider) {
  local_test_provider_for_testing_ = provider;
}

void ChromeBrowserPolicyConnector::MaybeApplyLocalTestPolicies(
    PrefService* local_state) {
  // Early return if the policy test page is disabled by any policy. This is
  // done because that policy is a profile level policy and we have not yet
  // loaded any profile to access its prefs.
  const auto& chrome_policies =
      GetPolicyService()->GetPolicies(policy::PolicyNamespace(
          policy::PolicyDomain::POLICY_DOMAIN_CHROME, std::string()));
  if (auto* policy_test_page_enabled = chrome_policies.GetValue(
          policy::key::kPolicyTestPageEnabled, base::Value::Type::BOOLEAN);
      policy_test_page_enabled && !policy_test_page_enabled->GetBool()) {
    return;
  }

  std::string policies_to_apply =
      local_state->GetString(policy_prefs::kLocalTestPoliciesForNextStartup);
  if (policies_to_apply.empty()) {
    return;
  }

  LocalTestPolicyProvider* test_provider =
      local_test_provider_for_testing_ ? static_cast<LocalTestPolicyProvider*>(
                                             local_test_provider_for_testing_)
                                       : local_test_provider_.get();
  test_provider->set_active(true);
  GetPolicyService()->UseLocalTestPolicyProvider(test_provider);
  test_provider->LoadJsonPolicies(policies_to_apply);
  local_state->ClearPref(policy_prefs::kLocalTestPoliciesForNextStartup);
}

#if !BUILDFLAG(IS_CHROMEOS)
void ChromeBrowserPolicyConnector::InitCloudManagementController(
    PrefService* local_state,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  chrome_browser_cloud_management_controller()->MaybeInit(local_state,
                                                          url_loader_factory);
}

void ChromeBrowserPolicyConnector::
    SetMachineLevelUserCloudPolicyManagerForTesting(
        MachineLevelUserCloudPolicyManager* manager) {
  machine_level_user_cloud_policy_manager_ = manager;
}

void ChromeBrowserPolicyConnector::SetProxyPolicyProviderForTesting(
    ProxyPolicyProvider* proxy_policy_provider) {
  proxy_policy_provider_ = proxy_policy_provider;
}
#endif  // !BUILDFLAG(IS_CHROMEOS)

bool ChromeBrowserPolicyConnector::IsCommandLineSwitchSupported() const {
  if (g_command_line_enabled_for_testing) {
    return true;
  }

  version_info::Channel channel = chrome::GetChannel();
  return channel != version_info::Channel::STABLE &&
         channel != version_info::Channel::BETA;
}

// static
void ChromeBrowserPolicyConnector::EnableCommandLineSupportForTesting() {
  g_command_line_enabled_for_testing = true;
}

// static
void ChromeBrowserPolicyConnector::EnablePlatformPolicySupport(
    const std::string& id) {
  *PlatformPolicyId() = id;
}

#if BUILDFLAG(IS_WIN)

// static
std::wstring ChromeBrowserPolicyConnector::GetPolicyKey() {
#if BUILDFLAG(ENABLE_CEF)
  const std::string& policy_id = *PlatformPolicyId();
  if (!policy_id.empty()) {
    return base::UTF8ToWide(policy_id);
  }
  return std::wstring();
#else
  return kRegistryChromePolicyKey;
#endif
}

#elif BUILDFLAG(IS_MAC)

// static
base::apple::ScopedCFTypeRef<CFStringRef>
ChromeBrowserPolicyConnector::GetBundleId() {
#if BUILDFLAG(ENABLE_CEF)
  const std::string& policy_id = *PlatformPolicyId();
  if (policy_id.empty()) {
    return base::apple::ScopedCFTypeRef<CFStringRef>();
  }

  return base::SysUTF8ToCFStringRef(policy_id);
#elif BUILDFLAG(GOOGLE_CHROME_BRANDING)
  // Explicitly access the "com.google.Chrome" bundle ID, no matter what this
  // app's bundle ID actually is. All channels of Chrome should obey the same
  // policies.
  return CFSTR("com.google.Chrome");
#else
  return base::SysUTF8ToCFStringRef(base::apple::BaseBundleID());
#endif
}

#elif BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)

// static
bool ChromeBrowserPolicyConnector::GetDirPolicyFilesPath(base::FilePath* path) {
#if BUILDFLAG(ENABLE_CEF)
  const std::string& policy_id = *PlatformPolicyId();
  if (policy_id.empty()) {
    return false;
  }

  base::FilePath policy_path(policy_id);
  if (!policy_path.IsAbsolute()) {
    return false;
  }

  *path = policy_path;
  return true;
#else
  return base::PathService::Get(chrome::DIR_POLICY_FILES, path);
#endif
}

#endif  // BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)

base::flat_set<std::string>
ChromeBrowserPolicyConnector::device_affiliation_ids() const {
  if (!device_affiliation_ids_for_testing_.empty()) {
    return device_affiliation_ids_for_testing_;
  }
#if !BUILDFLAG(IS_CHROMEOS)
  if (!machine_level_user_cloud_policy_manager_ ||
      !machine_level_user_cloud_policy_manager_->IsClientRegistered() ||
      !machine_level_user_cloud_policy_manager_->core() ||
      !machine_level_user_cloud_policy_manager_->core()->store() ||
      !machine_level_user_cloud_policy_manager_->core()->store()->policy()) {
    return {};
  }
  const auto& ids = machine_level_user_cloud_policy_manager_->core()
                        ->store()
                        ->policy()
                        ->device_affiliation_ids();
  return {ids.begin(), ids.end()};
#else
  return {};
#endif  // !BUILDFLAG(IS_CHROMEOS)
}

void ChromeBrowserPolicyConnector::SetDeviceAffiliatedIdsForTesting(
    const base::flat_set<std::string>& device_affiliation_ids) {
  device_affiliation_ids_for_testing_ = device_affiliation_ids;
}

std::vector<std::unique_ptr<policy::ConfigurationPolicyProvider>>
ChromeBrowserPolicyConnector::CreatePolicyProviders() {
  auto providers = BrowserPolicyConnector::CreatePolicyProviders();
  std::unique_ptr<ConfigurationPolicyProvider> platform_provider =
      CreatePlatformProvider();
  if (platform_provider) {
    platform_provider_ = platform_provider.get();
    // PlatformProvider should be before all other providers (highest priority).
    providers.insert(providers.begin(), std::move(platform_provider));
  }

#if !BUILDFLAG(IS_CHROMEOS)
  MaybeCreateCloudPolicyManager(&providers);
#endif  // !BUILDFLAG(IS_CHROMEOS)

  std::unique_ptr<CommandLinePolicyProvider> command_line_provider =
      CommandLinePolicyProvider::CreateIfAllowed(
          *base::CommandLine::ForCurrentProcess(), chrome::GetChannel());
  if (command_line_provider) {
    command_line_provider_ = command_line_provider.get();
    providers.push_back(std::move(command_line_provider));
  }

  local_test_provider_ =
      LocalTestPolicyProvider::CreateIfAllowed(chrome::GetChannel());
  if (local_test_provider_) {
    local_test_provider_->Init(GetSchemaRegistry());
  }

  return providers;
}

std::unique_ptr<ConfigurationPolicyProvider>
ChromeBrowserPolicyConnector::CreatePlatformProvider() {
#if BUILDFLAG(IS_WIN)
  const std::wstring policy_key = GetPolicyKey();
  if (policy_key.empty()) {
    return nullptr;
  }
  std::unique_ptr<AsyncPolicyLoader> loader(PolicyLoaderWin::Create(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT}),
      ManagementServiceFactory::GetForPlatform(), policy_key));
  return std::make_unique<AsyncPolicyProvider>(GetSchemaRegistry(),
                                               std::move(loader));
#elif BUILDFLAG(IS_MAC)
  base::apple::ScopedCFTypeRef<CFStringRef> bundle_id_scoper(GetBundleId());
  CFStringRef bundle_id = bundle_id_scoper.get();
  if (!bundle_id) {
    return nullptr;
  }
  auto loader = std::make_unique<PolicyLoaderMac>(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT}),
      ManagementServiceFactory::GetForPlatform(),
      PolicyLoaderMac::GetManagedPolicyPath(bundle_id),
      std::make_unique<MacPreferences>(), bundle_id);
  return std::make_unique<AsyncPolicyProvider>(GetSchemaRegistry(),
                                               std::move(loader));
#elif BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_CHROMEOS)
  base::FilePath config_dir_path;
  if (GetDirPolicyFilesPath(&config_dir_path)) {
    auto loader = std::make_unique<ConfigDirPolicyLoader>(
        base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::BEST_EFFORT}),
        config_dir_path, POLICY_SCOPE_MACHINE);
    return std::make_unique<AsyncPolicyProvider>(GetSchemaRegistry(),
                                                 std::move(loader));
  } else {
    return nullptr;
  }
#elif BUILDFLAG(IS_ANDROID)
  return std::make_unique<android::AndroidCombinedPolicyProvider>(
      GetSchemaRegistry());
#else
  return nullptr;
#endif
}

#if !BUILDFLAG(IS_CHROMEOS)
void ChromeBrowserPolicyConnector::MaybeCreateCloudPolicyManager(
    std::vector<std::unique_ptr<ConfigurationPolicyProvider>>* providers) {
  if (!chrome_browser_cloud_management_controller_->IsEnabled()) {
    return;
  }

  std::unique_ptr<ProxyPolicyProvider> proxy_policy_provider =
      std::make_unique<ProxyPolicyProvider>();
  proxy_policy_provider_ = proxy_policy_provider.get();
  providers->push_back(std::move(proxy_policy_provider));

  chrome_browser_cloud_management_controller_->DeferrableCreatePolicyManager(
      platform_provider_,
      base::BindOnce(&ChromeBrowserPolicyConnector::
                         OnMachineLevelCloudPolicyManagerCreated,
                     weak_factory_.GetWeakPtr()));
}

void ChromeBrowserPolicyConnector::OnMachineLevelCloudPolicyManagerCreated(
    std::unique_ptr<MachineLevelUserCloudPolicyManager>
        machine_level_user_cloud_policy_manager) {
  machine_level_user_cloud_policy_manager_ =
      machine_level_user_cloud_policy_manager.get();
  if (machine_level_user_cloud_policy_manager_) {
    machine_level_user_cloud_policy_manager_->Init(GetSchemaRegistry());
    proxy_policy_provider_->SetOwnedDelegate(
        std::move(machine_level_user_cloud_policy_manager));
  } else {
    // Explicitly transition out of ProxyPolicyProvider's Unspecified state
    // when no manager is created to unblock IsFirstPolicyLoadComplete() and
    // the startup policy snapshot.
    proxy_policy_provider_->SetUnownedDelegate(nullptr);
  }
}
#endif  // !BUILDFLAG(IS_CHROMEOS)

}  // namespace policy
