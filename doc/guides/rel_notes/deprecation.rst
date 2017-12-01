ABI and API Deprecation
=======================

See the :doc:`guidelines document for details of the ABI policy </contributing/versioning>`.
API and ABI deprecation notices are to be posted here.


Deprecation Notices
-------------------

* eal: several API and ABI changes are planned for ``rte_devargs`` in v18.02.
  The format of device command line parameters will change. The bus will need
  to be explicitly stated in the device declaration. The enum ``rte_devtype``
  was used to identify a bus and will disappear.
  The structure ``rte_devargs`` will change.
  The ``rte_devargs_list`` will be made private.
  The following functions are deprecated starting from 17.08 and will either be
  modified or removed in 18.02:

  - ``rte_eal_devargs_add``
  - ``rte_eal_devargs_type_count``
  - ``rte_eal_parse_devargs_str``, replaced by ``rte_eal_devargs_parse``

* pci: Several exposed functions are misnamed.
  The following functions are deprecated starting from v17.11 and are replaced:

  - ``eal_parse_pci_BDF`` replaced by ``rte_pci_addr_parse``
  - ``eal_parse_pci_DomBDF`` replaced by ``rte_pci_addr_parse``
  - ``rte_eal_compare_pci_addr`` replaced by ``rte_pci_addr_cmp``

* ethdev: a new Tx and Rx offload API was introduced on 17.11.
  In the new API, offloads are divided into per-port and per-queue offloads.
  Offloads are disabled by default and enabled per application request.
  The old offloads API is target to be deprecated on 18.05. This includes:

  - removal of ``ETH_TXQ_FLAGS_NO*`` flags.
  - removal of ``txq_flags`` field from ``rte_eth_txconf`` struct.
  - removal of the offloads bit-field from ``rte_eth_rxmode`` struct.

* ethdev: the legacy filter API, including
  ``rte_eth_dev_filter_supported()``, ``rte_eth_dev_filter_ctrl()`` as well
  as filter types MACVLAN, ETHERTYPE, FLEXIBLE, SYN, NTUPLE, TUNNEL, FDIR,
  HASH and L2_TUNNEL, is superseded by the generic flow API (rte_flow) in
  PMDs that implement the latter.
  Target release for removal of the legacy API will be defined once most
  PMDs have switched to rte_flow.

* ethdev: The device flag advertizing hotplug capability
  ``RTE_ETH_DEV_DETACHABLE`` is not needed anymore and will be removed in
  v17.11.
  This capability is verified upon calling the relevant hotplug functions in EAL
  by checking that the ``unplug`` ops is set in the bus. This verification is
  done by the EAL and not by the ``ethdev`` layer anymore. Users relying on this
  flag being present only have to remove their checks to follow the change.

* ethdev: new parameters - ``rte_security_capabilities`` and
  ``rte_security_ops`` will be added to ``rte_eth_dev_info`` and
  ``rte_eth_dev`` respectively  to support security operations like
  ipsec inline.

* cryptodev: new parameters - ``rte_security_capabilities`` and
  ``rte_security_ops`` will be added to ``rte_cryptodev_info`` and
  ``rte_cryptodev`` respectively to support security protocol offloaded
  operations.

* cryptodev: the following function is deprecated starting from 17.08 and will
  be removed in 17.11:

  - ``rte_cryptodev_create_vdev``

* librte_meter: The API will change to accommodate configuration profiles.
  Most of the API functions will have an additional opaque parameter.
