import template from './cloud-sync-modal.html'
import ko from 'knockout';
import { cloudSyncInfo } from 'model';
import { loadCloudSyncInfo, removeCloudSyncPolicy } from 'actions';
import { formatDuration } from 'utils';

const syncStatusMapping = Object.freeze({
    NOTSET:         { label: 'Not Set',         css: 'no-set'         },
    PENDING:        { label: 'Pending',         css: 'pending'        },
    SYNCING:        { label: 'Syncing',         css: 'syncing'        },
    PASUED:         { label: 'Paused',          css: 'paused'         },
    SYNCED:         { label: 'Synced',          css: 'synced'         },
    UNABLE:         { label: 'Unable To Sync',  css: 'unable-to-sync' }
});

class CloudSyncModalViewModel {
    constructor({ bucketName, onClose }) {
        this.sourceBucket = bucketName;
        this.onClose = onClose;

        this.syncStatus = ko.pureComputed(
            () => {
                if (!cloudSyncInfo()) {
                    return { label: 'N/A', css : '' };
                }

                return syncStatusMapping[cloudSyncInfo().status];
            }
        );

        this.targetEndpoint = ko.pureComputed(
            () => cloudSyncInfo() && cloudSyncInfo().endpoint
        );

        this.targetAccessKey = ko.pureComputed(
            () => cloudSyncInfo() && cloudSyncInfo().access_key
        );

        let policy = ko.pureComputed(
            () => cloudSyncInfo() && cloudSyncInfo().policy
        );

        this.targetBucket = ko.pureComputed(
            () => policy() && policy().target_bucket
        );

        this.syncFrequency = ko.pureComputed(
            () => policy() && `Every ${formatDuration(policy().schedule)}`
        );

        this.syncDirection = ko.pureComputed(
            () => {
                if (!policy()) {
                    return;
                }

                let { n2c_enabled, c2n_enabled } = policy();
                return n2c_enabled ?
                    (c2n_enabled ? 'Bi-Direcitonal' : 'Source to Target') :
                    'Target to Source';
            }
        );

        this.syncDeletions = ko.pureComputed(
            () => policy() && !policy().additions_only
        );

        loadCloudSyncInfo(ko.unwrap(this.sourceBucket));
    }

    removePolicy() {
        removeCloudSyncPolicy(ko.unwrap(this.sourceBucket));
        this.onClose();
    }

    close() {
        this.onClose()
    }
}

export default {
    viewModel: CloudSyncModalViewModel,
    template: template
}
