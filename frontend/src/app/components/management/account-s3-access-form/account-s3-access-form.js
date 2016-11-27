import template from './account-s3-access-form.html';
import Disposable from 'disposable';
import ko from 'knockout';
import { routeContext, systemInfo, accountS3ACL } from 'model';
import { loadAccountS3ACL } from 'actions';

class AccountS3AccessFormViewModel extends Disposable{
    constructor() {
        super();

        const account = ko.pureComputed(
            () => {
                if (!systemInfo()) {
                    return { access_keys: [] };
                }

                const email = routeContext().params.account;
                return systemInfo().accounts.find(
                    account => account.email === email
                );
            }
        );

        this.email = ko.pureComputed(
            () => account().email
        );

        this.s3AccessInfo = [
            {
                label: 'S3 Access',
                value: ko.pureComputed(
                    () => account().has_s3_access ? 'Enabled' : 'Disabled'
                )
            },
            {
                label: 'Permitted buckets',
                value: ko.pureComputed(
                    () => (accountS3ACL() || [])
                        .filter( ({ is_allowed }) => is_allowed )
                        .map( ({ bucket_name }) => bucket_name )
                        .join(', ')
                )
            }
        ];

        const keys = ko.pureComputed(
            () => account().access_keys[0] || {}
        );

        this.credentials = [
            {
                label: 'Access Key',
                value: ko.pureComputed( () => keys().access_key ),
                allowCopy: true
            },
            {
                label: 'Secret Key',
                value: ko.pureComputed( () => keys().secret_key ),
                allowCopy: true
            }
        ];

        account().email && loadAccountS3ACL(account().email);
        this.addToDisposeList(
            account.subscribe( ({ email }) => loadAccountS3ACL(email) )
        );

        this.isEditAccountS3AccessModalVisible = ko.observable();
    }

    showEditAccountS3AccessModal() {
        this.isEditAccountS3AccessModalVisible(true);
    }

    hideEditAccountS3AccessModal() {
        this.isEditAccountS3AccessModalVisible(false);
    }
}

export default {
    viewModel: AccountS3AccessFormViewModel,
    template: template
};
