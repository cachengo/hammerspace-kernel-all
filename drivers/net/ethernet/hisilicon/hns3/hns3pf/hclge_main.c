// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/if_vlan.h>
#include <net/rtnetlink.h>
#include "hclge_cmd.h"
#include "hclge_dcb.h"
#include "hclge_main.h"
#include "hclge_mbx.h"
#include "hclge_mdio.h"
#include "hclge_tm.h"
#include "hclge_err.h"
#include "hnae3.h"

#define HCLGE_NAME			"hclge"
#define HCLGE_STATS_READ(p, offset) (*((u64 *)((u8 *)(p) + (offset))))
#define HCLGE_MAC_STATS_FIELD_OFF(f) (offsetof(struct hclge_mac_stats, f))

#define HCLGE_BUF_SIZE_UNIT	256

static int hclge_set_mac_mtu(struct hclge_dev *hdev, int new_mps);
static int hclge_init_vlan_config(struct hclge_dev *hdev);
static int hclge_reset_ae_dev(struct hnae3_ae_dev *ae_dev);
static int hclge_set_umv_space(struct hclge_dev *hdev, u16 space_size,
			       u16 *allocated_size, bool is_alloc);

static struct hnae3_ae_algo ae_algo;

static const struct pci_device_id ae_algo_pci_tbl[] = {
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_GE), 0},
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_25GE), 0},
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_25GE_RDMA), 0},
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_25GE_RDMA_MACSEC), 0},
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_50GE_RDMA), 0},
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_50GE_RDMA_MACSEC), 0},
	{PCI_VDEVICE(HUAWEI, HNAE3_DEV_ID_100G_RDMA_MACSEC), 0},
	/* required last entry */
	{0, }
};

MODULE_DEVICE_TABLE(pci, ae_algo_pci_tbl);

static const u32 cmdq_reg_addr_list[] = {HCLGE_CMDQ_TX_ADDR_L_REG,
					 HCLGE_CMDQ_TX_ADDR_H_REG,
					 HCLGE_CMDQ_TX_DEPTH_REG,
					 HCLGE_CMDQ_TX_TAIL_REG,
					 HCLGE_CMDQ_TX_HEAD_REG,
					 HCLGE_CMDQ_RX_ADDR_L_REG,
					 HCLGE_CMDQ_RX_ADDR_H_REG,
					 HCLGE_CMDQ_RX_DEPTH_REG,
					 HCLGE_CMDQ_RX_TAIL_REG,
					 HCLGE_CMDQ_RX_HEAD_REG,
					 HCLGE_VECTOR0_CMDQ_SRC_REG,
					 HCLGE_CMDQ_INTR_STS_REG,
					 HCLGE_CMDQ_INTR_EN_REG,
					 HCLGE_CMDQ_INTR_GEN_REG};

static const u32 common_reg_addr_list[] = {HCLGE_MISC_VECTOR_REG_BASE,
					   HCLGE_VECTOR0_OTER_EN_REG,
					   HCLGE_MISC_RESET_STS_REG,
					   HCLGE_MISC_VECTOR_INT_STS,
					   HCLGE_GLOBAL_RESET_REG,
					   HCLGE_FUN_RST_ING,
					   HCLGE_GRO_EN_REG};

static const u32 ring_reg_addr_list[] = {HCLGE_RING_RX_ADDR_L_REG,
					 HCLGE_RING_RX_ADDR_H_REG,
					 HCLGE_RING_RX_BD_NUM_REG,
					 HCLGE_RING_RX_BD_LENGTH_REG,
					 HCLGE_RING_RX_MERGE_EN_REG,
					 HCLGE_RING_RX_TAIL_REG,
					 HCLGE_RING_RX_HEAD_REG,
					 HCLGE_RING_RX_FBD_NUM_REG,
					 HCLGE_RING_RX_OFFSET_REG,
					 HCLGE_RING_RX_FBD_OFFSET_REG,
					 HCLGE_RING_RX_STASH_REG,
					 HCLGE_RING_RX_BD_ERR_REG,
					 HCLGE_RING_TX_ADDR_L_REG,
					 HCLGE_RING_TX_ADDR_H_REG,
					 HCLGE_RING_TX_BD_NUM_REG,
					 HCLGE_RING_TX_PRIORITY_REG,
					 HCLGE_RING_TX_TC_REG,
					 HCLGE_RING_TX_MERGE_EN_REG,
					 HCLGE_RING_TX_TAIL_REG,
					 HCLGE_RING_TX_HEAD_REG,
					 HCLGE_RING_TX_FBD_NUM_REG,
					 HCLGE_RING_TX_OFFSET_REG,
					 HCLGE_RING_TX_EBD_NUM_REG,
					 HCLGE_RING_TX_EBD_OFFSET_REG,
					 HCLGE_RING_TX_BD_ERR_REG,
					 HCLGE_RING_EN_REG};

static const u32 tqp_intr_reg_addr_list[] = {HCLGE_TQP_INTR_CTRL_REG,
					     HCLGE_TQP_INTR_GL0_REG,
					     HCLGE_TQP_INTR_GL1_REG,
					     HCLGE_TQP_INTR_GL2_REG,
					     HCLGE_TQP_INTR_RL_REG};

static const char hns3_nic_test_strs[][ETH_GSTRING_LEN] = {
	"App    Loopback test",
	"Serdes serial Loopback test",
	"Serdes parallel Loopback test",
	"Phy    Loopback test"
};

static const struct hclge_comm_stats_str g_mac_stats_string[] = {
	{"mac_tx_mac_pause_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_mac_pause_num)},
	{"mac_rx_mac_pause_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_mac_pause_num)},
	{"mac_tx_control_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_ctrl_pkt_num)},
	{"mac_rx_control_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_ctrl_pkt_num)},
	{"mac_tx_pfc_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pause_pkt_num)},
	{"mac_tx_pfc_pri0_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri0_pkt_num)},
	{"mac_tx_pfc_pri1_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri1_pkt_num)},
	{"mac_tx_pfc_pri2_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri2_pkt_num)},
	{"mac_tx_pfc_pri3_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri3_pkt_num)},
	{"mac_tx_pfc_pri4_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri4_pkt_num)},
	{"mac_tx_pfc_pri5_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri5_pkt_num)},
	{"mac_tx_pfc_pri6_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri6_pkt_num)},
	{"mac_tx_pfc_pri7_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_pfc_pri7_pkt_num)},
	{"mac_rx_pfc_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pause_pkt_num)},
	{"mac_rx_pfc_pri0_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri0_pkt_num)},
	{"mac_rx_pfc_pri1_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri1_pkt_num)},
	{"mac_rx_pfc_pri2_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri2_pkt_num)},
	{"mac_rx_pfc_pri3_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri3_pkt_num)},
	{"mac_rx_pfc_pri4_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri4_pkt_num)},
	{"mac_rx_pfc_pri5_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri5_pkt_num)},
	{"mac_rx_pfc_pri6_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri6_pkt_num)},
	{"mac_rx_pfc_pri7_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_pfc_pri7_pkt_num)},
	{"mac_tx_total_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_total_pkt_num)},
	{"mac_tx_total_oct_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_total_oct_num)},
	{"mac_tx_good_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_good_pkt_num)},
	{"mac_tx_bad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_bad_pkt_num)},
	{"mac_tx_good_oct_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_good_oct_num)},
	{"mac_tx_bad_oct_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_bad_oct_num)},
	{"mac_tx_uni_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_uni_pkt_num)},
	{"mac_tx_multi_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_multi_pkt_num)},
	{"mac_tx_broad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_broad_pkt_num)},
	{"mac_tx_undersize_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_undersize_pkt_num)},
	{"mac_tx_oversize_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_oversize_pkt_num)},
	{"mac_tx_64_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_64_oct_pkt_num)},
	{"mac_tx_65_127_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_65_127_oct_pkt_num)},
	{"mac_tx_128_255_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_128_255_oct_pkt_num)},
	{"mac_tx_256_511_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_256_511_oct_pkt_num)},
	{"mac_tx_512_1023_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_512_1023_oct_pkt_num)},
	{"mac_tx_1024_1518_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_1024_1518_oct_pkt_num)},
	{"mac_tx_1519_2047_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_1519_2047_oct_pkt_num)},
	{"mac_tx_2048_4095_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_2048_4095_oct_pkt_num)},
	{"mac_tx_4096_8191_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_4096_8191_oct_pkt_num)},
	{"mac_tx_8192_9216_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_8192_9216_oct_pkt_num)},
	{"mac_tx_9217_12287_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_9217_12287_oct_pkt_num)},
	{"mac_tx_12288_16383_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_12288_16383_oct_pkt_num)},
	{"mac_tx_1519_max_good_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_1519_max_good_oct_pkt_num)},
	{"mac_tx_1519_max_bad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_1519_max_bad_oct_pkt_num)},
	{"mac_rx_total_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_total_pkt_num)},
	{"mac_rx_total_oct_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_total_oct_num)},
	{"mac_rx_good_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_good_pkt_num)},
	{"mac_rx_bad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_bad_pkt_num)},
	{"mac_rx_good_oct_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_good_oct_num)},
	{"mac_rx_bad_oct_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_bad_oct_num)},
	{"mac_rx_uni_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_uni_pkt_num)},
	{"mac_rx_multi_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_multi_pkt_num)},
	{"mac_rx_broad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_broad_pkt_num)},
	{"mac_rx_undersize_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_undersize_pkt_num)},
	{"mac_rx_oversize_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_oversize_pkt_num)},
	{"mac_rx_64_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_64_oct_pkt_num)},
	{"mac_rx_65_127_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_65_127_oct_pkt_num)},
	{"mac_rx_128_255_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_128_255_oct_pkt_num)},
	{"mac_rx_256_511_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_256_511_oct_pkt_num)},
	{"mac_rx_512_1023_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_512_1023_oct_pkt_num)},
	{"mac_rx_1024_1518_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_1024_1518_oct_pkt_num)},
	{"mac_rx_1519_2047_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_1519_2047_oct_pkt_num)},
	{"mac_rx_2048_4095_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_2048_4095_oct_pkt_num)},
	{"mac_rx_4096_8191_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_4096_8191_oct_pkt_num)},
	{"mac_rx_8192_9216_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_8192_9216_oct_pkt_num)},
	{"mac_rx_9217_12287_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_9217_12287_oct_pkt_num)},
	{"mac_rx_12288_16383_oct_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_12288_16383_oct_pkt_num)},
	{"mac_rx_1519_max_good_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_1519_max_good_oct_pkt_num)},
	{"mac_rx_1519_max_bad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_1519_max_bad_oct_pkt_num)},

	{"mac_tx_fragment_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_fragment_pkt_num)},
	{"mac_tx_undermin_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_undermin_pkt_num)},
	{"mac_tx_jabber_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_jabber_pkt_num)},
	{"mac_tx_err_all_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_err_all_pkt_num)},
	{"mac_tx_from_app_good_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_from_app_good_pkt_num)},
	{"mac_tx_from_app_bad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_tx_from_app_bad_pkt_num)},
	{"mac_rx_fragment_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_fragment_pkt_num)},
	{"mac_rx_undermin_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_undermin_pkt_num)},
	{"mac_rx_jabber_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_jabber_pkt_num)},
	{"mac_rx_fcs_err_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_fcs_err_pkt_num)},
	{"mac_rx_send_app_good_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_send_app_good_pkt_num)},
	{"mac_rx_send_app_bad_pkt_num",
		HCLGE_MAC_STATS_FIELD_OFF(mac_rx_send_app_bad_pkt_num)}
};

static const struct hclge_mac_mgr_tbl_entry_cmd hclge_mgr_table[] = {
	{
		.flags = HCLGE_MAC_MGR_MASK_VLAN_B,
		.ethter_type = cpu_to_le16(HCLGE_MAC_ETHERTYPE_LLDP),
		.mac_addr_hi32 = cpu_to_le32(htonl(0x0180C200)),
		.mac_addr_lo16 = cpu_to_le16(htons(0x000E)),
		.i_port_bitmap = 0x1,
	},
};

static const u8 hclge_hash_key[] = {
	0x6D, 0x5A, 0x56, 0xDA, 0x25, 0x5B, 0x0E, 0xC2,
	0x41, 0x67, 0x25, 0x3D, 0x43, 0xA3, 0x8F, 0xB0,
	0xD0, 0xCA, 0x2B, 0xCB, 0xAE, 0x7B, 0x30, 0xB4,
	0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30, 0xF2, 0x0C,
	0x6A, 0x42, 0xB7, 0x3B, 0xBE, 0xAC, 0x01, 0xFA
};

static int hclge_mac_update_stats_defective(struct hclge_dev *hdev)
{
#define HCLGE_MAC_CMD_NUM 21

	u64 *data = (u64 *)(&hdev->hw_stats.mac_stats);
	struct hclge_desc desc[HCLGE_MAC_CMD_NUM];
	__le64 *desc_data;
	int i, k, n;
	int ret;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_STATS_MAC, true);
	ret = hclge_cmd_send(&hdev->hw, desc, HCLGE_MAC_CMD_NUM);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Get MAC pkt stats fail, status = %d.\n", ret);

		return ret;
	}

	for (i = 0; i < HCLGE_MAC_CMD_NUM; i++) {
		/* for special opcode 0032, only the first desc has the head */
		if (unlikely(i == 0)) {
			desc_data = (__le64 *)(&desc[i].data[0]);
			n = HCLGE_RD_FIRST_STATS_NUM;
		} else {
			desc_data = (__le64 *)(&desc[i]);
			n = HCLGE_RD_OTHER_STATS_NUM;
		}

		for (k = 0; k < n; k++) {
			*data += le64_to_cpu(*desc_data);
			data++;
			desc_data++;
		}
	}

	return 0;
}

static int hclge_mac_update_stats_complete(struct hclge_dev *hdev, u32 desc_num)
{
	u64 *data = (u64 *)(&hdev->hw_stats.mac_stats);
	struct hclge_desc *desc;
	__le64 *desc_data;
	u16 i, k, n;
	int ret;

	desc = kcalloc(desc_num, sizeof(struct hclge_desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_STATS_MAC_ALL, true);
	ret = hclge_cmd_send(&hdev->hw, desc, desc_num);
	if (ret) {
		kfree(desc);
		return ret;
	}

	for (i = 0; i < desc_num; i++) {
		/* for special opcode 0034, only the first desc has the head */
		if (i == 0) {
			desc_data = (__le64 *)(&desc[i].data[0]);
			n = HCLGE_RD_FIRST_STATS_NUM;
		} else {
			desc_data = (__le64 *)(&desc[i]);
			n = HCLGE_RD_OTHER_STATS_NUM;
		}

		for (k = 0; k < n; k++) {
			*data += le64_to_cpu(*desc_data);
			data++;
			desc_data++;
		}
	}

	kfree(desc);

	return 0;
}

static int hclge_mac_query_reg_num(struct hclge_dev *hdev, u32 *desc_num)
{
	struct hclge_desc desc;
	__le32 *desc_data;
	u32 reg_num;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_QUERY_MAC_REG_NUM, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		return ret;

	desc_data = (__le32 *)(&desc.data[0]);
	reg_num = le32_to_cpu(*desc_data);

	*desc_num = 1 + ((reg_num - 3) >> 2) +
		    (u32)(((reg_num - 3) & 0x3) ? 1 : 0);

	return 0;
}

static int hclge_mac_update_stats(struct hclge_dev *hdev)
{
	u32 desc_num;
	int ret;

	ret = hclge_mac_query_reg_num(hdev, &desc_num);

	/* The firmware supports the new statistics acquisition method */
	if (!ret)
		ret = hclge_mac_update_stats_complete(hdev, desc_num);
	else if (ret == -EOPNOTSUPP)
		ret = hclge_mac_update_stats_defective(hdev);
	else
		dev_err(&hdev->pdev->dev, "query mac reg num fail!\n");

	return ret;
}

static int hclge_tqps_update_stats(struct hnae3_handle *handle)
{
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hnae3_queue *queue;
	struct hclge_desc desc[1];
	struct hclge_tqp *tqp;
	int ret, i;

	for (i = 0; i < kinfo->num_tqps; i++) {
		queue = handle->kinfo.tqp[i];
		tqp = container_of(queue, struct hclge_tqp, q);
		/* command : HCLGE_OPC_QUERY_IGU_STAT */
		hclge_cmd_setup_basic_desc(&desc[0],
					   HCLGE_OPC_QUERY_RX_STATUS,
					   true);

		desc[0].data[0] = cpu_to_le32((tqp->index & 0x1ff));
		ret = hclge_cmd_send(&hdev->hw, desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"Query tqp stat fail, status = %d,queue = %d\n",
				ret,	i);
			return ret;
		}
		tqp->tqp_stats.rcb_rx_ring_pktnum_rcd +=
			le32_to_cpu(desc[0].data[1]);
	}

	for (i = 0; i < kinfo->num_tqps; i++) {
		queue = handle->kinfo.tqp[i];
		tqp = container_of(queue, struct hclge_tqp, q);
		/* command : HCLGE_OPC_QUERY_IGU_STAT */
		hclge_cmd_setup_basic_desc(&desc[0],
					   HCLGE_OPC_QUERY_TX_STATUS,
					   true);

		desc[0].data[0] = cpu_to_le32((tqp->index & 0x1ff));
		ret = hclge_cmd_send(&hdev->hw, desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"Query tqp stat fail, status = %d,queue = %d\n",
				ret, i);
			return ret;
		}
		tqp->tqp_stats.rcb_tx_ring_pktnum_rcd +=
			le32_to_cpu(desc[0].data[1]);
	}

	return 0;
}

static u64 *hclge_tqps_get_stats(struct hnae3_handle *handle, u64 *data)
{
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;
	struct hclge_tqp *tqp;
	u64 *buff = data;
	int i;

	for (i = 0; i < kinfo->num_tqps; i++) {
		tqp = container_of(kinfo->tqp[i], struct hclge_tqp, q);
		*buff++ = tqp->tqp_stats.rcb_tx_ring_pktnum_rcd;
	}

	for (i = 0; i < kinfo->num_tqps; i++) {
		tqp = container_of(kinfo->tqp[i], struct hclge_tqp, q);
		*buff++ = tqp->tqp_stats.rcb_rx_ring_pktnum_rcd;
	}

	return buff;
}

static int hclge_tqps_get_sset_count(struct hnae3_handle *handle, int stringset)
{
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;

	return kinfo->num_tqps * (2);
}

static u8 *hclge_tqps_get_strings(struct hnae3_handle *handle, u8 *data)
{
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;
	u8 *buff = data;
	int i = 0;

	for (i = 0; i < kinfo->num_tqps; i++) {
		struct hclge_tqp *tqp = container_of(handle->kinfo.tqp[i],
			struct hclge_tqp, q);
		snprintf(buff, ETH_GSTRING_LEN, "txq%d_pktnum_rcd",
			 tqp->index);
		buff = buff + ETH_GSTRING_LEN;
	}

	for (i = 0; i < kinfo->num_tqps; i++) {
		struct hclge_tqp *tqp = container_of(kinfo->tqp[i],
			struct hclge_tqp, q);
		snprintf(buff, ETH_GSTRING_LEN, "rxq%d_pktnum_rcd",
			 tqp->index);
		buff = buff + ETH_GSTRING_LEN;
	}

	return buff;
}

static u64 *hclge_comm_get_stats(void *comm_stats,
				 const struct hclge_comm_stats_str strs[],
				 int size, u64 *data)
{
	u64 *buf = data;
	u32 i;

	for (i = 0; i < size; i++)
		buf[i] = HCLGE_STATS_READ(comm_stats, strs[i].offset);

	return buf + size;
}

static u8 *hclge_comm_get_strings(u32 stringset,
				  const struct hclge_comm_stats_str strs[],
				  int size, u8 *data)
{
	char *buff = (char *)data;
	u32 i;

	if (stringset != ETH_SS_STATS)
		return buff;

	for (i = 0; i < size; i++) {
		snprintf(buff, ETH_GSTRING_LEN, "%s", strs[i].desc);
		buff = buff + ETH_GSTRING_LEN;
	}

	return (u8 *)buff;
}

static void hclge_update_stats_for_all(struct hclge_dev *hdev)
{
	struct hnae3_handle *handle;
	int status;

	handle = &hdev->vport[0].nic;
	if (handle->client) {
		status = hclge_tqps_update_stats(handle);
		if (status) {
			dev_err(&hdev->pdev->dev,
				"Update TQPS stats fail, status = %d.\n",
				status);
		}
	}

	status = hclge_mac_update_stats(hdev);
	if (status)
		dev_err(&hdev->pdev->dev,
			"Update MAC stats fail, status = %d.\n", status);
}

static void hclge_update_stats(struct hnae3_handle *handle,
			       struct net_device_stats *net_stats)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int status;

	if (test_and_set_bit(HCLGE_STATE_STATISTICS_UPDATING, &hdev->state))
		return;

	status = hclge_mac_update_stats(hdev);
	if (status)
		dev_err(&hdev->pdev->dev,
			"Update MAC stats fail, status = %d.\n",
			status);

	status = hclge_tqps_update_stats(handle);
	if (status)
		dev_err(&hdev->pdev->dev,
			"Update TQPS stats fail, status = %d.\n",
			status);

	clear_bit(HCLGE_STATE_STATISTICS_UPDATING, &hdev->state);
}

static int hclge_get_sset_count(struct hnae3_handle *handle, int stringset)
{
#define HCLGE_LOOPBACK_TEST_FLAGS (HNAE3_SUPPORT_APP_LOOPBACK |\
		HNAE3_SUPPORT_PHY_LOOPBACK |\
		HNAE3_SUPPORT_SERDES_SERIAL_LOOPBACK |\
		HNAE3_SUPPORT_SERDES_PARALLEL_LOOPBACK)

	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int count = 0;

	/* Loopback test support rules:
	 * mac: only GE mode support
	 * serdes: all mac mode will support include GE/XGE/LGE/CGE
	 * phy: only support when phy device exist on board
	 */
	if (stringset == ETH_SS_TEST) {
		/* clear loopback bit flags at first */
		handle->flags = (handle->flags & (~HCLGE_LOOPBACK_TEST_FLAGS));
		if (hdev->pdev->revision >= 0x21 ||
		    hdev->hw.mac.speed == HCLGE_MAC_SPEED_10M ||
		    hdev->hw.mac.speed == HCLGE_MAC_SPEED_100M ||
		    hdev->hw.mac.speed == HCLGE_MAC_SPEED_1G) {
			count += 1;
			handle->flags |= HNAE3_SUPPORT_APP_LOOPBACK;
		}

		count += 2;
		handle->flags |= HNAE3_SUPPORT_SERDES_SERIAL_LOOPBACK;
		handle->flags |= HNAE3_SUPPORT_SERDES_PARALLEL_LOOPBACK;
	} else if (stringset == ETH_SS_STATS) {
		count = ARRAY_SIZE(g_mac_stats_string) +
			hclge_tqps_get_sset_count(handle, stringset);
	}

	return count;
}

static void hclge_get_strings(struct hnae3_handle *handle,
			      u32 stringset,
			      u8 *data)
{
	u8 *p = (char *)data;
	int size;

	if (stringset == ETH_SS_STATS) {
		size = ARRAY_SIZE(g_mac_stats_string);
		p = hclge_comm_get_strings(stringset,
					   g_mac_stats_string,
					   size,
					   p);
		p = hclge_tqps_get_strings(handle, p);
	} else if (stringset == ETH_SS_TEST) {
		if (handle->flags & HNAE3_SUPPORT_APP_LOOPBACK) {
			memcpy(p,
			       hns3_nic_test_strs[HNAE3_LOOP_APP],
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		if (handle->flags & HNAE3_SUPPORT_SERDES_SERIAL_LOOPBACK) {
			memcpy(p,
			       hns3_nic_test_strs[HNAE3_LOOP_SERIAL_SERDES],
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		if (handle->flags & HNAE3_SUPPORT_SERDES_PARALLEL_LOOPBACK) {
			memcpy(p,
			       hns3_nic_test_strs[HNAE3_LOOP_PARALLEL_SERDES],
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		if (handle->flags & HNAE3_SUPPORT_PHY_LOOPBACK) {
			memcpy(p,
			       hns3_nic_test_strs[HNAE3_LOOP_PHY],
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
	}
}

static void hclge_get_stats(struct hnae3_handle *handle, u64 *data)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	u64 *p;

	p = hclge_comm_get_stats(&hdev->hw_stats.mac_stats,
				 g_mac_stats_string,
				 ARRAY_SIZE(g_mac_stats_string),
				 data);
	p = hclge_tqps_get_stats(handle, p);
}

static int hclge_parse_func_status(struct hclge_dev *hdev,
				   struct hclge_func_status_cmd *status)
{
	if (!(status->pf_state & HCLGE_PF_STATE_DONE))
		return -EINVAL;

	/* Set the pf to main pf */
	if (status->pf_state & HCLGE_PF_STATE_MAIN)
		hdev->flag |= HCLGE_FLAG_MAIN;
	else
		hdev->flag &= ~HCLGE_FLAG_MAIN;

	return 0;
}

static int hclge_query_function_status(struct hclge_dev *hdev)
{
	struct hclge_func_status_cmd *req;
	struct hclge_desc desc;
	int timeout = 0;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_QUERY_FUNC_STATUS, true);
	req = (struct hclge_func_status_cmd *)desc.data;

	do {
		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"query function status failed %d.\n",
				ret);

			return ret;
		}

		/* Check pf reset is done */
		if (req->pf_state)
			break;
		usleep_range(1000, 2000);
	} while (timeout++ < 5);

	ret = hclge_parse_func_status(hdev, req);

	return ret;
}

static int hclge_query_pf_resource(struct hclge_dev *hdev)
{
	struct hclge_pf_res_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_QUERY_PF_RSRC, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"query pf resource failed %d.\n", ret);
		return ret;
	}

	req = (struct hclge_pf_res_cmd *)desc.data;
	hdev->num_tqps = __le16_to_cpu(req->tqp_num);
	hdev->pkt_buf_size = __le16_to_cpu(req->buf_size) << HCLGE_BUF_UNIT_S;

	if (req->tx_buf_size)
		hdev->tx_buf_size =
			__le16_to_cpu(req->tx_buf_size) << HCLGE_BUF_UNIT_S;
	else
		hdev->tx_buf_size = HCLGE_DEFAULT_TX_BUF;

	hdev->tx_buf_size = roundup(hdev->tx_buf_size, HCLGE_BUF_SIZE_UNIT);

	if (req->dv_buf_size)
		hdev->dv_buf_size =
			__le16_to_cpu(req->dv_buf_size) << HCLGE_BUF_UNIT_S;
	else
		hdev->dv_buf_size = HCLGE_DEFAULT_DV;

	hdev->dv_buf_size = roundup(hdev->dv_buf_size, HCLGE_BUF_SIZE_UNIT);

	if (hnae3_dev_roce_supported(hdev)) {
		hdev->roce_base_msix_offset =
		hnae3_get_field(__le16_to_cpu(req->msixcap_localid_ba_rocee),
				HCLGE_MSIX_OFT_ROCEE_M, HCLGE_MSIX_OFT_ROCEE_S);
		hdev->num_roce_msi =
		hnae3_get_field(__le16_to_cpu(req->pf_intr_vector_number),
				HCLGE_PF_VEC_NUM_M, HCLGE_PF_VEC_NUM_S);

		/* PF should have NIC vectors and Roce vectors,
		 * NIC vectors are queued before Roce vectors.
		 */
		hdev->num_msi = hdev->num_roce_msi  +
				hdev->roce_base_msix_offset;
	} else {
		hdev->num_msi =
		hnae3_get_field(__le16_to_cpu(req->pf_intr_vector_number),
				HCLGE_PF_VEC_NUM_M, HCLGE_PF_VEC_NUM_S);
	}

	return 0;
}

static int hclge_parse_speed(int speed_cmd, int *speed)
{
	switch (speed_cmd) {
	case 6:
		*speed = HCLGE_MAC_SPEED_10M;
		break;
	case 7:
		*speed = HCLGE_MAC_SPEED_100M;
		break;
	case 0:
		*speed = HCLGE_MAC_SPEED_1G;
		break;
	case 1:
		*speed = HCLGE_MAC_SPEED_10G;
		break;
	case 2:
		*speed = HCLGE_MAC_SPEED_25G;
		break;
	case 3:
		*speed = HCLGE_MAC_SPEED_40G;
		break;
	case 4:
		*speed = HCLGE_MAC_SPEED_50G;
		break;
	case 5:
		*speed = HCLGE_MAC_SPEED_100G;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void hclge_parse_fiber_link_mode(struct hclge_dev *hdev,
					u8 speed_ability)
{
	unsigned long *supported = hdev->hw.mac.supported;

	if (speed_ability & HCLGE_SUPPORT_1G_BIT)
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseX_Full_BIT,
				 supported);

	if (speed_ability & HCLGE_SUPPORT_10G_BIT)
		linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseSR_Full_BIT,
				 supported);

	if (speed_ability & HCLGE_SUPPORT_25G_BIT)
		linkmode_set_bit(ETHTOOL_LINK_MODE_25000baseSR_Full_BIT,
				 supported);

	if (speed_ability & HCLGE_SUPPORT_50G_BIT)
		linkmode_set_bit(ETHTOOL_LINK_MODE_50000baseSR2_Full_BIT,
				 supported);

	if (speed_ability & HCLGE_SUPPORT_100G_BIT)
		linkmode_set_bit(ETHTOOL_LINK_MODE_100000baseSR4_Full_BIT,
				 supported);

	linkmode_set_bit(ETHTOOL_LINK_MODE_FIBRE_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, supported);
}

static void hclge_parse_copper_link_mode(struct hclge_dev *hdev,
					 u8 speed_ability)
{
	unsigned long *supported = hdev->hw.mac.supported;

	/* default to support all speed for GE port */
	if (!speed_ability)
		speed_ability = HCLGE_SUPPORT_GE;

	if (speed_ability & HCLGE_SUPPORT_1G_BIT)
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
				 supported);

	if (speed_ability & HCLGE_SUPPORT_100M_BIT) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT,
				 supported);
		linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT,
				 supported);
	}

	if (speed_ability & HCLGE_SUPPORT_10M_BIT) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, supported);
		linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, supported);
	}

	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, supported);
}

static void hclge_parse_link_mode(struct hclge_dev *hdev, u8 speed_ability)
{
	u8 media_type = hdev->hw.mac.media_type;

	if (media_type == HNAE3_MEDIA_TYPE_FIBER)
		hclge_parse_fiber_link_mode(hdev, speed_ability);
	else if (media_type == HNAE3_MEDIA_TYPE_COPPER)
		hclge_parse_copper_link_mode(hdev, speed_ability);
}

static void hclge_parse_cfg(struct hclge_cfg *cfg, struct hclge_desc *desc)
{
	struct hclge_cfg_param_cmd *req;
	u64 mac_addr_tmp_high;
	u64 mac_addr_tmp;
	int i;

	req = (struct hclge_cfg_param_cmd *)desc[0].data;

	/* get the configuration */
	cfg->vmdq_vport_num = hnae3_get_field(__le32_to_cpu(req->param[0]),
					      HCLGE_CFG_VMDQ_M,
					      HCLGE_CFG_VMDQ_S);
	cfg->tc_num = hnae3_get_field(__le32_to_cpu(req->param[0]),
				      HCLGE_CFG_TC_NUM_M, HCLGE_CFG_TC_NUM_S);
	cfg->tqp_desc_num = hnae3_get_field(__le32_to_cpu(req->param[0]),
					    HCLGE_CFG_TQP_DESC_N_M,
					    HCLGE_CFG_TQP_DESC_N_S);

	cfg->phy_addr = hnae3_get_field(__le32_to_cpu(req->param[1]),
					HCLGE_CFG_PHY_ADDR_M,
					HCLGE_CFG_PHY_ADDR_S);
	cfg->media_type = hnae3_get_field(__le32_to_cpu(req->param[1]),
					  HCLGE_CFG_MEDIA_TP_M,
					  HCLGE_CFG_MEDIA_TP_S);
	cfg->rx_buf_len = hnae3_get_field(__le32_to_cpu(req->param[1]),
					  HCLGE_CFG_RX_BUF_LEN_M,
					  HCLGE_CFG_RX_BUF_LEN_S);
	/* get mac_address */
	mac_addr_tmp = __le32_to_cpu(req->param[2]);
	mac_addr_tmp_high = hnae3_get_field(__le32_to_cpu(req->param[3]),
					    HCLGE_CFG_MAC_ADDR_H_M,
					    HCLGE_CFG_MAC_ADDR_H_S);

	mac_addr_tmp |= (mac_addr_tmp_high << 31) << 1;

	cfg->default_speed = hnae3_get_field(__le32_to_cpu(req->param[3]),
					     HCLGE_CFG_DEFAULT_SPEED_M,
					     HCLGE_CFG_DEFAULT_SPEED_S);
	cfg->rss_size_max = hnae3_get_field(__le32_to_cpu(req->param[3]),
					    HCLGE_CFG_RSS_SIZE_M,
					    HCLGE_CFG_RSS_SIZE_S);

	for (i = 0; i < ETH_ALEN; i++)
		cfg->mac_addr[i] = (mac_addr_tmp >> (8 * i)) & 0xff;

	req = (struct hclge_cfg_param_cmd *)desc[1].data;
	cfg->numa_node_map = __le32_to_cpu(req->param[0]);

	cfg->speed_ability = hnae3_get_field(__le32_to_cpu(req->param[1]),
					     HCLGE_CFG_SPEED_ABILITY_M,
					     HCLGE_CFG_SPEED_ABILITY_S);
	cfg->umv_space = hnae3_get_field(__le32_to_cpu(req->param[1]),
					 HCLGE_CFG_UMV_TBL_SPACE_M,
					 HCLGE_CFG_UMV_TBL_SPACE_S);
	if (!cfg->umv_space)
		cfg->umv_space = HCLGE_DEFAULT_UMV_SPACE_PER_PF;
}

/* hclge_get_cfg: query the static parameter from flash
 * @hdev: pointer to struct hclge_dev
 * @hcfg: the config structure to be getted
 */
static int hclge_get_cfg(struct hclge_dev *hdev, struct hclge_cfg *hcfg)
{
	struct hclge_desc desc[HCLGE_PF_CFG_DESC_NUM];
	struct hclge_cfg_param_cmd *req;
	int i, ret;

	for (i = 0; i < HCLGE_PF_CFG_DESC_NUM; i++) {
		u32 offset = 0;

		req = (struct hclge_cfg_param_cmd *)desc[i].data;
		hclge_cmd_setup_basic_desc(&desc[i], HCLGE_OPC_GET_CFG_PARAM,
					   true);
		hnae3_set_field(offset, HCLGE_CFG_OFFSET_M,
				HCLGE_CFG_OFFSET_S, i * HCLGE_CFG_RD_LEN_BYTES);
		/* Len should be united by 4 bytes when send to hardware */
		hnae3_set_field(offset, HCLGE_CFG_RD_LEN_M, HCLGE_CFG_RD_LEN_S,
				HCLGE_CFG_RD_LEN_BYTES / HCLGE_CFG_RD_LEN_UNIT);
		req->offset = cpu_to_le32(offset);
	}

	ret = hclge_cmd_send(&hdev->hw, desc, HCLGE_PF_CFG_DESC_NUM);
	if (ret) {
		dev_err(&hdev->pdev->dev, "get config failed %d.\n", ret);
		return ret;
	}

	hclge_parse_cfg(hcfg, desc);

	return 0;
}

static int hclge_get_cap(struct hclge_dev *hdev)
{
	int ret;

	ret = hclge_query_function_status(hdev);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"query function status error %d.\n", ret);
		return ret;
	}

	/* get pf resource */
	ret = hclge_query_pf_resource(hdev);
	if (ret)
		dev_err(&hdev->pdev->dev, "query pf resource error %d.\n", ret);

	return ret;
}

static int hclge_configure(struct hclge_dev *hdev)
{
	struct hclge_cfg cfg;
	int ret, i;

	ret = hclge_get_cfg(hdev, &cfg);
	if (ret) {
		dev_err(&hdev->pdev->dev, "get mac mode error %d.\n", ret);
		return ret;
	}

	hdev->num_vmdq_vport = cfg.vmdq_vport_num;
	hdev->base_tqp_pid = 0;
	hdev->rss_size_max = cfg.rss_size_max;
	hdev->rx_buf_len = cfg.rx_buf_len;
	ether_addr_copy(hdev->hw.mac.mac_addr, cfg.mac_addr);
	hdev->hw.mac.media_type = cfg.media_type;
	hdev->hw.mac.phy_addr = cfg.phy_addr;
	hdev->num_tx_desc = cfg.tqp_desc_num;
	hdev->num_rx_desc = cfg.tqp_desc_num;
	hdev->tm_info.num_pg = 1;
	hdev->tc_max = cfg.tc_num;
	hdev->tm_info.hw_pfc_map = 0;
	hdev->wanted_umv_size = cfg.umv_space;

	if (hnae3_dev_fd_supported(hdev))
		hdev->fd_en = true;

	ret = hclge_parse_speed(cfg.default_speed, &hdev->hw.mac.speed);
	if (ret) {
		dev_err(&hdev->pdev->dev, "Get wrong speed ret=%d.\n", ret);
		return ret;
	}

	hclge_parse_link_mode(hdev, cfg.speed_ability);

	if ((hdev->tc_max > HNAE3_MAX_TC) ||
	    (hdev->tc_max < 1)) {
		dev_warn(&hdev->pdev->dev, "TC num = %d.\n",
			 hdev->tc_max);
		hdev->tc_max = 1;
	}

	/* Dev does not support DCB */
	if (!hnae3_dev_dcb_supported(hdev)) {
		hdev->tc_max = 1;
		hdev->pfc_max = 0;
	} else {
		hdev->pfc_max = hdev->tc_max;
	}

	hdev->tm_info.num_tc = 1;

	/* Currently not support uncontiuous tc */
	for (i = 0; i < hdev->tm_info.num_tc; i++)
		hnae3_set_bit(hdev->hw_tc_map, i, 1);

	hdev->tx_sch_mode = HCLGE_FLAG_TC_BASE_SCH_MODE;

	return ret;
}

static int hclge_config_tso(struct hclge_dev *hdev, int tso_mss_min,
			    int tso_mss_max)
{
	struct hclge_cfg_tso_status_cmd *req;
	struct hclge_desc desc;
	u16 tso_mss;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_TSO_GENERIC_CONFIG, false);

	req = (struct hclge_cfg_tso_status_cmd *)desc.data;

	tso_mss = 0;
	hnae3_set_field(tso_mss, HCLGE_TSO_MSS_MIN_M,
			HCLGE_TSO_MSS_MIN_S, tso_mss_min);
	req->tso_mss_min = cpu_to_le16(tso_mss);

	tso_mss = 0;
	hnae3_set_field(tso_mss, HCLGE_TSO_MSS_MIN_M,
			HCLGE_TSO_MSS_MIN_S, tso_mss_max);
	req->tso_mss_max = cpu_to_le16(tso_mss);

	return hclge_cmd_send(&hdev->hw, &desc, 1);
}

static int hclge_config_gro(struct hclge_dev *hdev, bool en)
{
	struct hclge_cfg_gro_status_cmd *req;
	struct hclge_desc desc;
	int ret;

	if (!hnae3_dev_gro_supported(hdev))
		return 0;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_GRO_GENERIC_CONFIG, false);
	req = (struct hclge_cfg_gro_status_cmd *)desc.data;

	req->gro_en = cpu_to_le16(en ? 1 : 0);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"GRO hardware config cmd failed, ret = %d\n", ret);

	return ret;
}

static int hclge_alloc_tqps(struct hclge_dev *hdev)
{
	struct hclge_tqp *tqp;
	int i;

	hdev->htqp = devm_kcalloc(&hdev->pdev->dev, hdev->num_tqps,
				  sizeof(struct hclge_tqp), GFP_KERNEL);
	if (!hdev->htqp)
		return -ENOMEM;

	tqp = hdev->htqp;

	for (i = 0; i < hdev->num_tqps; i++) {
		tqp->dev = &hdev->pdev->dev;
		tqp->index = i;

		tqp->q.ae_algo = &ae_algo;
		tqp->q.buf_size = hdev->rx_buf_len;
		tqp->q.tx_desc_num = hdev->num_tx_desc;
		tqp->q.rx_desc_num = hdev->num_rx_desc;
		tqp->q.io_base = hdev->hw.io_base + HCLGE_TQP_REG_OFFSET +
			i * HCLGE_TQP_REG_SIZE;

		tqp++;
	}

	return 0;
}

static int hclge_map_tqps_to_func(struct hclge_dev *hdev, u16 func_id,
				  u16 tqp_pid, u16 tqp_vid, bool is_pf)
{
	struct hclge_tqp_map_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_SET_TQP_MAP, false);

	req = (struct hclge_tqp_map_cmd *)desc.data;
	req->tqp_id = cpu_to_le16(tqp_pid);
	req->tqp_vf = func_id;
	req->tqp_flag = !is_pf << HCLGE_TQP_MAP_TYPE_B |
			1 << HCLGE_TQP_MAP_EN_B;
	req->tqp_vid = cpu_to_le16(tqp_vid);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev, "TQP map failed %d.\n", ret);

	return ret;
}

static int  hclge_assign_tqp(struct hclge_vport *vport, u16 num_tqps)
{
	struct hnae3_knic_private_info *kinfo = &vport->nic.kinfo;
	struct hclge_dev *hdev = vport->back;
	int i, alloced;

	for (i = 0, alloced = 0; i < hdev->num_tqps &&
	     alloced < num_tqps; i++) {
		if (!hdev->htqp[i].alloced) {
			hdev->htqp[i].q.handle = &vport->nic;
			hdev->htqp[i].q.tqp_index = alloced;
			hdev->htqp[i].q.tx_desc_num = kinfo->num_tx_desc;
			hdev->htqp[i].q.rx_desc_num = kinfo->num_rx_desc;
			kinfo->tqp[alloced] = &hdev->htqp[i].q;
			hdev->htqp[i].alloced = true;
			alloced++;
		}
	}
	vport->alloc_tqps = alloced;
	kinfo->rss_size = min_t(u16, hdev->rss_size_max,
				vport->alloc_tqps / hdev->tm_info.num_tc);

	return 0;
}

static int hclge_knic_setup(struct hclge_vport *vport, u16 num_tqps,
			    u16 num_tx_desc, u16 num_rx_desc)

{
	struct hnae3_handle *nic = &vport->nic;
	struct hnae3_knic_private_info *kinfo = &nic->kinfo;
	struct hclge_dev *hdev = vport->back;
	int ret;

	kinfo->num_tx_desc = num_tx_desc;
	kinfo->num_rx_desc = num_rx_desc;

	kinfo->rx_buf_len = hdev->rx_buf_len;

	kinfo->tqp = devm_kcalloc(&hdev->pdev->dev, num_tqps,
				  sizeof(struct hnae3_queue *), GFP_KERNEL);
	if (!kinfo->tqp)
		return -ENOMEM;

	ret = hclge_assign_tqp(vport, num_tqps);
	if (ret)
		dev_err(&hdev->pdev->dev, "fail to assign TQPs %d.\n", ret);

	return ret;
}

static int hclge_map_tqp_to_vport(struct hclge_dev *hdev,
				  struct hclge_vport *vport)
{
	struct hnae3_handle *nic = &vport->nic;
	struct hnae3_knic_private_info *kinfo;
	u16 i;

	kinfo = &nic->kinfo;
	for (i = 0; i < vport->alloc_tqps; i++) {
		struct hclge_tqp *q =
			container_of(kinfo->tqp[i], struct hclge_tqp, q);
		bool is_pf;
		int ret;

		is_pf = !(vport->vport_id);
		ret = hclge_map_tqps_to_func(hdev, vport->vport_id, q->index,
					     i, is_pf);
		if (ret)
			return ret;
	}

	return 0;
}

static int hclge_map_tqp(struct hclge_dev *hdev)
{
	struct hclge_vport *vport = hdev->vport;
	u16 i, num_vport;

	num_vport = hdev->num_vmdq_vport + hdev->num_req_vfs + 1;
	for (i = 0; i < num_vport; i++)	{
		int ret;

		ret = hclge_map_tqp_to_vport(hdev, vport);
		if (ret)
			return ret;

		vport++;
	}

	return 0;
}

static void hclge_unic_setup(struct hclge_vport *vport, u16 num_tqps)
{
	/* this would be initialized later */
}

static int hclge_vport_setup(struct hclge_vport *vport, u16 num_tqps)
{
	struct hnae3_handle *nic = &vport->nic;
	struct hclge_dev *hdev = vport->back;
	int ret;

	nic->pdev = hdev->pdev;
	nic->ae_algo = &ae_algo;
	nic->numa_node_mask = hdev->numa_node_mask;

	if (hdev->ae_dev->dev_type == HNAE3_DEV_KNIC) {
		ret = hclge_knic_setup(vport, num_tqps,
				       hdev->num_tx_desc, hdev->num_rx_desc);

		if (ret) {
			dev_err(&hdev->pdev->dev, "knic setup failed %d\n",
				ret);
			return ret;
		}
	} else {
		hclge_unic_setup(vport, num_tqps);
	}

	return 0;
}

static int hclge_alloc_vport(struct hclge_dev *hdev)
{
	struct pci_dev *pdev = hdev->pdev;
	struct hclge_vport *vport;
	u32 tqp_main_vport;
	u32 tqp_per_vport;
	int num_vport, i;
	int ret;

	/* We need to alloc a vport for main NIC of PF */
	num_vport = hdev->num_vmdq_vport + hdev->num_req_vfs + 1;

	if (hdev->num_tqps < num_vport) {
		dev_err(&hdev->pdev->dev, "tqps(%d) is less than vports(%d)",
			hdev->num_tqps, num_vport);
		return -EINVAL;
	}

	/* Alloc the same number of TQPs for every vport */
	tqp_per_vport = hdev->num_tqps / num_vport;
	tqp_main_vport = tqp_per_vport + hdev->num_tqps % num_vport;

	vport = devm_kcalloc(&pdev->dev, num_vport, sizeof(struct hclge_vport),
			     GFP_KERNEL);
	if (!vport)
		return -ENOMEM;

	hdev->vport = vport;
	hdev->num_alloc_vport = num_vport;

	if (IS_ENABLED(CONFIG_PCI_IOV))
		hdev->num_alloc_vfs = hdev->num_req_vfs;

	for (i = 0; i < num_vport; i++) {
		vport->back = hdev;
		vport->vport_id = i;
		vport->mps = HCLGE_MAC_DEFAULT_FRAME;
		INIT_LIST_HEAD(&vport->vlan_list);
		INIT_LIST_HEAD(&vport->uc_mac_list);
		INIT_LIST_HEAD(&vport->mc_mac_list);

		if (i == 0)
			ret = hclge_vport_setup(vport, tqp_main_vport);
		else
			ret = hclge_vport_setup(vport, tqp_per_vport);
		if (ret) {
			dev_err(&pdev->dev,
				"vport setup failed for vport %d, %d\n",
				i, ret);
			return ret;
		}

		vport++;
	}

	return 0;
}

static int  hclge_cmd_alloc_tx_buff(struct hclge_dev *hdev,
				    struct hclge_pkt_buf_alloc *buf_alloc)
{
/* TX buffer size is unit by 128 byte */
#define HCLGE_BUF_SIZE_UNIT_SHIFT	7
#define HCLGE_BUF_SIZE_UPDATE_EN_MSK	BIT(15)
	struct hclge_tx_buff_alloc_cmd *req;
	struct hclge_desc desc;
	int ret;
	u8 i;

	req = (struct hclge_tx_buff_alloc_cmd *)desc.data;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_TX_BUFF_ALLOC, 0);
	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		u32 buf_size = buf_alloc->priv_buf[i].tx_buf_size;

		req->tx_pkt_buff[i] =
			cpu_to_le16((buf_size >> HCLGE_BUF_SIZE_UNIT_SHIFT) |
				     HCLGE_BUF_SIZE_UPDATE_EN_MSK);
	}

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev, "tx buffer alloc cmd failed %d.\n",
			ret);

	return ret;
}

static int hclge_tx_buffer_alloc(struct hclge_dev *hdev,
				 struct hclge_pkt_buf_alloc *buf_alloc)
{
	int ret = hclge_cmd_alloc_tx_buff(hdev, buf_alloc);

	if (ret)
		dev_err(&hdev->pdev->dev, "tx buffer alloc failed %d\n", ret);

	return ret;
}

static int hclge_get_tc_num(struct hclge_dev *hdev)
{
	int i, cnt = 0;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++)
		if (hdev->hw_tc_map & BIT(i))
			cnt++;
	return cnt;
}

static int hclge_get_pfc_enalbe_num(struct hclge_dev *hdev)
{
	int i, cnt = 0;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++)
		if (hdev->hw_tc_map & BIT(i) &&
		    hdev->tm_info.hw_pfc_map & BIT(i))
			cnt++;
	return cnt;
}

/* Get the number of pfc enabled TCs, which have private buffer */
static int hclge_get_pfc_priv_num(struct hclge_dev *hdev,
				  struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_priv_buf *priv;
	int i, cnt = 0;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		priv = &buf_alloc->priv_buf[i];
		if ((hdev->tm_info.hw_pfc_map & BIT(i)) &&
		    priv->enable)
			cnt++;
	}

	return cnt;
}

/* Get the number of pfc disabled TCs, which have private buffer */
static int hclge_get_no_pfc_priv_num(struct hclge_dev *hdev,
				     struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_priv_buf *priv;
	int i, cnt = 0;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		priv = &buf_alloc->priv_buf[i];
		if (hdev->hw_tc_map & BIT(i) &&
		    !(hdev->tm_info.hw_pfc_map & BIT(i)) &&
		    priv->enable)
			cnt++;
	}

	return cnt;
}

static u32 hclge_get_rx_priv_buff_alloced(struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_priv_buf *priv;
	u32 rx_priv = 0;
	int i;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		priv = &buf_alloc->priv_buf[i];
		if (priv->enable)
			rx_priv += priv->buf_size;
	}
	return rx_priv;
}

static u32 hclge_get_tx_buff_alloced(struct hclge_pkt_buf_alloc *buf_alloc)
{
	u32 i, total_tx_size = 0;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++)
		total_tx_size += buf_alloc->priv_buf[i].tx_buf_size;

	return total_tx_size;
}

static bool  hclge_is_rx_buf_ok(struct hclge_dev *hdev,
				struct hclge_pkt_buf_alloc *buf_alloc,
				u32 rx_all)
{
	u32 shared_buf_min, shared_buf_tc, shared_std;
	int tc_num, pfc_enable_num;
	u32 shared_buf, aligned_mps;
	u32 rx_priv;
	int i;

	tc_num = hclge_get_tc_num(hdev);
	pfc_enable_num = hclge_get_pfc_enalbe_num(hdev);
	aligned_mps = roundup(hdev->mps, HCLGE_BUF_SIZE_UNIT);

	if (hnae3_dev_dcb_supported(hdev))
		shared_buf_min = 2 * aligned_mps + hdev->dv_buf_size;
	else
		shared_buf_min = aligned_mps + HCLGE_NON_DCB_ADDITIONAL_BUF
					+ hdev->dv_buf_size;

	shared_buf_tc = pfc_enable_num * aligned_mps +
			(tc_num - pfc_enable_num) * aligned_mps / 2 +
			aligned_mps;
	shared_std = roundup(max_t(u32, shared_buf_min, shared_buf_tc),
			     HCLGE_BUF_SIZE_UNIT);

	rx_priv = hclge_get_rx_priv_buff_alloced(buf_alloc);
	if (rx_all < rx_priv + shared_std)
		return false;

	shared_buf = rounddown(rx_all - rx_priv, HCLGE_BUF_SIZE_UNIT);
	buf_alloc->s_buf.buf_size = shared_buf;
	if (hnae3_dev_dcb_supported(hdev)) {
		buf_alloc->s_buf.self.high = shared_buf - hdev->dv_buf_size;
		buf_alloc->s_buf.self.low = buf_alloc->s_buf.self.high
			- roundup(aligned_mps / 2, HCLGE_BUF_SIZE_UNIT);
	} else {
		buf_alloc->s_buf.self.high = aligned_mps +
						HCLGE_NON_DCB_ADDITIONAL_BUF;
		buf_alloc->s_buf.self.low =
			roundup(aligned_mps / 2, HCLGE_BUF_SIZE_UNIT);
	}

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		if ((hdev->hw_tc_map & BIT(i)) &&
		    (hdev->tm_info.hw_pfc_map & BIT(i))) {
			buf_alloc->s_buf.tc_thrd[i].low = aligned_mps;
			buf_alloc->s_buf.tc_thrd[i].high = 2 * aligned_mps;
		} else {
			buf_alloc->s_buf.tc_thrd[i].low = 0;
			buf_alloc->s_buf.tc_thrd[i].high = aligned_mps;
		}
	}

	return true;
}

static int hclge_tx_buffer_calc(struct hclge_dev *hdev,
				struct hclge_pkt_buf_alloc *buf_alloc)
{
	u32 i, total_size;

	total_size = hdev->pkt_buf_size;

	/* alloc tx buffer for all enabled tc */
	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		struct hclge_priv_buf *priv = &buf_alloc->priv_buf[i];

		if (hdev->hw_tc_map & BIT(i)) {
			if (total_size < hdev->tx_buf_size)
				return -ENOMEM;

			priv->tx_buf_size = hdev->tx_buf_size;
		} else {
			priv->tx_buf_size = 0;
		}

		total_size -= priv->tx_buf_size;
	}

	return 0;
}

static bool hclge_rx_buf_calc_all(struct hclge_dev *hdev, bool max,
				  struct hclge_pkt_buf_alloc *buf_alloc)
{
	u32 rx_all = hdev->pkt_buf_size - hclge_get_tx_buff_alloced(buf_alloc);
	u32 aligned_mps = round_up(hdev->mps, HCLGE_BUF_SIZE_UNIT);
	int i;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		struct hclge_priv_buf *priv = &buf_alloc->priv_buf[i];

		priv->enable = 0;
		priv->wl.low = 0;
		priv->wl.high = 0;
		priv->buf_size = 0;

		if (!(hdev->hw_tc_map & BIT(i)))
			continue;

		priv->enable = 1;

		if (hdev->tm_info.hw_pfc_map & BIT(i)) {
			priv->wl.low = max ? aligned_mps : 256;
			priv->wl.high = roundup(priv->wl.low + aligned_mps,
						HCLGE_BUF_SIZE_UNIT);
		} else {
			priv->wl.low = 0;
			priv->wl.high = max ? (aligned_mps * 2) : aligned_mps;
		}

		priv->buf_size = priv->wl.high + hdev->dv_buf_size;
	}

	return hclge_is_rx_buf_ok(hdev, buf_alloc, rx_all);
}

static bool hclge_drop_nopfc_buf_till_fit(struct hclge_dev *hdev,
					  struct hclge_pkt_buf_alloc *buf_alloc)
{
	u32 rx_all = hdev->pkt_buf_size - hclge_get_tx_buff_alloced(buf_alloc);
	int no_pfc_priv_num = hclge_get_no_pfc_priv_num(hdev, buf_alloc);
	int i;

	/* let the last to be cleared first */
	for (i = HCLGE_MAX_TC_NUM - 1; i >= 0; i--) {
		struct hclge_priv_buf *priv = &buf_alloc->priv_buf[i];

		if (hdev->hw_tc_map & BIT(i) &&
		    !(hdev->tm_info.hw_pfc_map & BIT(i))) {
			/* Clear the no pfc TC private buffer */
			priv->wl.low = 0;
			priv->wl.high = 0;
			priv->buf_size = 0;
			priv->enable = 0;
			no_pfc_priv_num--;
		}

		if (hclge_is_rx_buf_ok(hdev, buf_alloc, rx_all) ||
		    no_pfc_priv_num == 0)
			break;
	}

	return hclge_is_rx_buf_ok(hdev, buf_alloc, rx_all);
}

static bool hclge_drop_pfc_buf_till_fit(struct hclge_dev *hdev,
					struct hclge_pkt_buf_alloc *buf_alloc)
{
	u32 rx_all = hdev->pkt_buf_size - hclge_get_tx_buff_alloced(buf_alloc);
	int pfc_priv_num = hclge_get_pfc_priv_num(hdev, buf_alloc);
	int i;

	/* let the last to be cleared first */
	for (i = HCLGE_MAX_TC_NUM - 1; i >= 0; i--) {
		struct hclge_priv_buf *priv = &buf_alloc->priv_buf[i];

		if (hdev->hw_tc_map & BIT(i) &&
		    hdev->tm_info.hw_pfc_map & BIT(i)) {
			/* Reduce the number of pfc TC with private buffer */
			priv->wl.low = 0;
			priv->enable = 0;
			priv->wl.high = 0;
			priv->buf_size = 0;
			pfc_priv_num--;
		}

		if (hclge_is_rx_buf_ok(hdev, buf_alloc, rx_all) ||
		    pfc_priv_num == 0)
			break;
	}

	return hclge_is_rx_buf_ok(hdev, buf_alloc, rx_all);
}

/* hclge_rx_buffer_calc: calculate the rx private buffer size for all TCs
 * @hdev: pointer to struct hclge_dev
 * @buf_alloc: pointer to buffer calculation data
 * @return: 0: calculate sucessful, negative: fail
 */
static int hclge_rx_buffer_calc(struct hclge_dev *hdev,
				struct hclge_pkt_buf_alloc *buf_alloc)
{
	/* When DCB is not supported, rx private buffer is not allocated. */
	if (!hnae3_dev_dcb_supported(hdev)) {
		u32 rx_all = hdev->pkt_buf_size;

		rx_all -= hclge_get_tx_buff_alloced(buf_alloc);
		if (!hclge_is_rx_buf_ok(hdev, buf_alloc, rx_all))
			return -ENOMEM;

		return 0;
	}

	if (hclge_rx_buf_calc_all(hdev, true, buf_alloc))
		return 0;

	/* try to decrease the buffer size */
	if (hclge_rx_buf_calc_all(hdev, false, buf_alloc))
		return 0;

	if (hclge_drop_nopfc_buf_till_fit(hdev, buf_alloc))
		return 0;

	if (hclge_drop_pfc_buf_till_fit(hdev, buf_alloc))
		return 0;

	return -ENOMEM;
}

static int hclge_rx_priv_buf_alloc(struct hclge_dev *hdev,
				   struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_rx_priv_buff_cmd *req;
	struct hclge_desc desc;
	int ret;
	int i;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RX_PRIV_BUFF_ALLOC, false);
	req = (struct hclge_rx_priv_buff_cmd *)desc.data;

	/* Alloc private buffer TCs */
	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		struct hclge_priv_buf *priv = &buf_alloc->priv_buf[i];

		req->buf_num[i] =
			cpu_to_le16(priv->buf_size >> HCLGE_BUF_UNIT_S);
		req->buf_num[i] |=
			cpu_to_le16(1 << HCLGE_TC0_PRI_BUF_EN_B);
	}

	req->shared_buf =
		cpu_to_le16((buf_alloc->s_buf.buf_size >> HCLGE_BUF_UNIT_S) |
			    (1 << HCLGE_TC0_PRI_BUF_EN_B));

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"rx private buffer alloc cmd failed %d\n", ret);

	return ret;
}

static int hclge_rx_priv_wl_config(struct hclge_dev *hdev,
				   struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_rx_priv_wl_buf *req;
	struct hclge_priv_buf *priv;
	struct hclge_desc desc[2];
	int i, j;
	int ret;

	for (i = 0; i < 2; i++) {
		hclge_cmd_setup_basic_desc(&desc[i], HCLGE_OPC_RX_PRIV_WL_ALLOC,
					   false);
		req = (struct hclge_rx_priv_wl_buf *)desc[i].data;

		/* The first descriptor set the NEXT bit to 1 */
		if (i == 0)
			desc[i].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		else
			desc[i].flag &= ~cpu_to_le16(HCLGE_CMD_FLAG_NEXT);

		for (j = 0; j < HCLGE_TC_NUM_ONE_DESC; j++) {
			u32 idx = i * HCLGE_TC_NUM_ONE_DESC + j;

			priv = &buf_alloc->priv_buf[idx];
			req->tc_wl[j].high =
				cpu_to_le16(priv->wl.high >> HCLGE_BUF_UNIT_S);
			req->tc_wl[j].high |=
				cpu_to_le16(BIT(HCLGE_RX_PRIV_EN_B));
			req->tc_wl[j].low =
				cpu_to_le16(priv->wl.low >> HCLGE_BUF_UNIT_S);
			req->tc_wl[j].low |=
				 cpu_to_le16(BIT(HCLGE_RX_PRIV_EN_B));
		}
	}

	/* Send 2 descriptor at one time */
	ret = hclge_cmd_send(&hdev->hw, desc, 2);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"rx private waterline config cmd failed %d\n",
			ret);
	return ret;
}

static int hclge_common_thrd_config(struct hclge_dev *hdev,
				    struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_shared_buf *s_buf = &buf_alloc->s_buf;
	struct hclge_rx_com_thrd *req;
	struct hclge_desc desc[2];
	struct hclge_tc_thrd *tc;
	int i, j;
	int ret;

	for (i = 0; i < 2; i++) {
		hclge_cmd_setup_basic_desc(&desc[i],
					   HCLGE_OPC_RX_COM_THRD_ALLOC, false);
		req = (struct hclge_rx_com_thrd *)&desc[i].data;

		/* The first descriptor set the NEXT bit to 1 */
		if (i == 0)
			desc[i].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		else
			desc[i].flag &= ~cpu_to_le16(HCLGE_CMD_FLAG_NEXT);

		for (j = 0; j < HCLGE_TC_NUM_ONE_DESC; j++) {
			tc = &s_buf->tc_thrd[i * HCLGE_TC_NUM_ONE_DESC + j];

			req->com_thrd[j].high =
				cpu_to_le16(tc->high >> HCLGE_BUF_UNIT_S);
			req->com_thrd[j].high |=
				 cpu_to_le16(BIT(HCLGE_RX_PRIV_EN_B));
			req->com_thrd[j].low =
				cpu_to_le16(tc->low >> HCLGE_BUF_UNIT_S);
			req->com_thrd[j].low |=
				 cpu_to_le16(BIT(HCLGE_RX_PRIV_EN_B));
		}
	}

	/* Send 2 descriptors at one time */
	ret = hclge_cmd_send(&hdev->hw, desc, 2);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"common threshold config cmd failed %d\n", ret);
	return ret;
}

static int hclge_common_wl_config(struct hclge_dev *hdev,
				  struct hclge_pkt_buf_alloc *buf_alloc)
{
	struct hclge_shared_buf *buf = &buf_alloc->s_buf;
	struct hclge_rx_com_wl *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RX_COM_WL_ALLOC, false);

	req = (struct hclge_rx_com_wl *)desc.data;
	req->com_wl.high = cpu_to_le16(buf->self.high >> HCLGE_BUF_UNIT_S);
	req->com_wl.high |=  cpu_to_le16(BIT(HCLGE_RX_PRIV_EN_B));

	req->com_wl.low = cpu_to_le16(buf->self.low >> HCLGE_BUF_UNIT_S);
	req->com_wl.low |=  cpu_to_le16(BIT(HCLGE_RX_PRIV_EN_B));

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"common waterline config cmd failed %d\n", ret);

	return ret;
}

int hclge_buffer_alloc(struct hclge_dev *hdev)
{
	struct hclge_pkt_buf_alloc *pkt_buf;
	int ret;

	pkt_buf = kzalloc(sizeof(*pkt_buf), GFP_KERNEL);
	if (!pkt_buf)
		return -ENOMEM;

	ret = hclge_tx_buffer_calc(hdev, pkt_buf);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"could not calc tx buffer size for all TCs %d\n", ret);
		goto out;
	}

	ret = hclge_tx_buffer_alloc(hdev, pkt_buf);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"could not alloc tx buffers %d\n", ret);
		goto out;
	}

	ret = hclge_rx_buffer_calc(hdev, pkt_buf);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"could not calc rx priv buffer size for all TCs %d\n",
			ret);
		goto out;
	}

	ret = hclge_rx_priv_buf_alloc(hdev, pkt_buf);
	if (ret) {
		dev_err(&hdev->pdev->dev, "could not alloc rx priv buffer %d\n",
			ret);
		goto out;
	}

	if (hnae3_dev_dcb_supported(hdev)) {
		ret = hclge_rx_priv_wl_config(hdev, pkt_buf);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"could not configure rx private waterline %d\n",
				ret);
			goto out;
		}

		ret = hclge_common_thrd_config(hdev, pkt_buf);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"could not configure common threshold %d\n",
				ret);
			goto out;
		}
	}

	ret = hclge_common_wl_config(hdev, pkt_buf);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"could not configure common waterline %d\n", ret);

out:
	kfree(pkt_buf);
	return ret;
}

static int hclge_init_roce_base_info(struct hclge_vport *vport)
{
	struct hnae3_handle *roce = &vport->roce;
	struct hnae3_handle *nic = &vport->nic;

	roce->rinfo.num_vectors = vport->back->num_roce_msi;

	if (vport->back->num_msi_left < vport->roce.rinfo.num_vectors ||
	    vport->back->num_msi_left == 0)
		return -EINVAL;

	roce->rinfo.base_vector = vport->back->roce_base_vector;

	roce->rinfo.netdev = nic->kinfo.netdev;
	roce->rinfo.roce_io_base = vport->back->hw.io_base;

	roce->pdev = nic->pdev;
	roce->ae_algo = nic->ae_algo;
	roce->numa_node_mask = nic->numa_node_mask;

	return 0;
}

static int hclge_init_msi(struct hclge_dev *hdev)
{
	struct pci_dev *pdev = hdev->pdev;
	int vectors;
	int i;

	vectors = pci_alloc_irq_vectors(pdev, 1, hdev->num_msi,
					PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (vectors < 0) {
		dev_err(&pdev->dev,
			"failed(%d) to allocate MSI/MSI-X vectors\n",
			vectors);
		return vectors;
	}
	if (vectors < hdev->num_msi)
		dev_warn(&hdev->pdev->dev,
			 "requested %d MSI/MSI-X, but allocated %d MSI/MSI-X\n",
			 hdev->num_msi, vectors);

	hdev->num_msi = vectors;
	hdev->num_msi_left = vectors;
	hdev->base_msi_vector = pdev->irq;
	hdev->roce_base_vector = hdev->base_msi_vector +
				hdev->roce_base_msix_offset;

	hdev->vector_status = devm_kcalloc(&pdev->dev, hdev->num_msi,
					   sizeof(u16), GFP_KERNEL);
	if (!hdev->vector_status) {
		pci_free_irq_vectors(pdev);
		return -ENOMEM;
	}

	for (i = 0; i < hdev->num_msi; i++)
		hdev->vector_status[i] = HCLGE_INVALID_VPORT;

	hdev->vector_irq = devm_kcalloc(&pdev->dev, hdev->num_msi,
					sizeof(int), GFP_KERNEL);
	if (!hdev->vector_irq) {
		pci_free_irq_vectors(pdev);
		return -ENOMEM;
	}

	return 0;
}

static u8 hclge_check_speed_dup(u8 duplex, int speed)
{

	if (!(speed == HCLGE_MAC_SPEED_10M || speed == HCLGE_MAC_SPEED_100M))
		duplex = HCLGE_MAC_FULL;

	return duplex;
}

static int hclge_cfg_mac_speed_dup_hw(struct hclge_dev *hdev, int speed,
				      u8 duplex)
{
	struct hclge_config_mac_speed_dup_cmd *req;
	struct hclge_desc desc;
	int ret;

	req = (struct hclge_config_mac_speed_dup_cmd *)desc.data;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CONFIG_SPEED_DUP, false);

	hnae3_set_bit(req->speed_dup, HCLGE_CFG_DUPLEX_B, !!duplex);

	switch (speed) {
	case HCLGE_MAC_SPEED_10M:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 6);
		break;
	case HCLGE_MAC_SPEED_100M:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 7);
		break;
	case HCLGE_MAC_SPEED_1G:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 0);
		break;
	case HCLGE_MAC_SPEED_10G:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 1);
		break;
	case HCLGE_MAC_SPEED_25G:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 2);
		break;
	case HCLGE_MAC_SPEED_40G:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 3);
		break;
	case HCLGE_MAC_SPEED_50G:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 4);
		break;
	case HCLGE_MAC_SPEED_100G:
		hnae3_set_field(req->speed_dup, HCLGE_CFG_SPEED_M,
				HCLGE_CFG_SPEED_S, 5);
		break;
	default:
		dev_err(&hdev->pdev->dev, "invalid speed (%d)\n", speed);
		return -EINVAL;
	}

	hnae3_set_bit(req->mac_change_fec_en, HCLGE_CFG_MAC_SPEED_CHANGE_EN_B,
		      1);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"mac speed/duplex config cmd failed %d.\n", ret);
		return ret;
	}

	return 0;
}

int hclge_cfg_mac_speed_dup(struct hclge_dev *hdev, int speed, u8 duplex)
{
	int ret;

	duplex = hclge_check_speed_dup(duplex, speed);
	if (hdev->hw.mac.speed == speed && hdev->hw.mac.duplex == duplex)
		return 0;

	ret = hclge_cfg_mac_speed_dup_hw(hdev, speed, duplex);
	if (ret)
		return ret;

	hdev->hw.mac.speed = speed;
	hdev->hw.mac.duplex = duplex;

	return 0;
}

static int hclge_cfg_mac_speed_dup_h(struct hnae3_handle *handle, int speed,
				     u8 duplex)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hclge_cfg_mac_speed_dup(hdev, speed, duplex);
}

static int hclge_set_autoneg_en(struct hclge_dev *hdev, bool enable)
{
	struct hclge_config_auto_neg_cmd *req;
	struct hclge_desc desc;
	u32 flag = 0;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CONFIG_AN_MODE, false);

	req = (struct hclge_config_auto_neg_cmd *)desc.data;
	hnae3_set_bit(flag, HCLGE_MAC_CFG_AN_EN_B, !!enable);
	req->cfg_an_cmd_flag = cpu_to_le32(flag);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev, "auto neg set cmd failed %d.\n",
			ret);

	return ret;
}

static int hclge_set_autoneg(struct hnae3_handle *handle, bool enable)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hclge_set_autoneg_en(hdev, enable);
}

static int hclge_get_autoneg(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct phy_device *phydev = hdev->hw.mac.phydev;

	if (phydev)
		return phydev->autoneg;

	return hdev->hw.mac.autoneg;
}

static int hclge_mac_init(struct hclge_dev *hdev)
{
	struct hclge_mac *mac = &hdev->hw.mac;
	int ret;

	hdev->support_sfp_query = true;
	hdev->hw.mac.duplex = HCLGE_MAC_FULL;
	ret = hclge_cfg_mac_speed_dup_hw(hdev, hdev->hw.mac.speed,
					 hdev->hw.mac.duplex);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Config mac speed dup fail ret=%d\n", ret);
		return ret;
	}

	mac->link = 0;

	ret = hclge_set_mac_mtu(hdev, hdev->mps);
	if (ret) {
		dev_err(&hdev->pdev->dev, "set mtu failed ret=%d\n", ret);
		return ret;
	}

	ret = hclge_buffer_alloc(hdev);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"allocate buffer fail, ret=%d\n", ret);

	return ret;
}

static void hclge_mbx_task_schedule(struct hclge_dev *hdev)
{
	if (!test_and_set_bit(HCLGE_STATE_MBX_SERVICE_SCHED, &hdev->state))
		schedule_work(&hdev->mbx_service_task);
}

static void hclge_reset_task_schedule(struct hclge_dev *hdev)
{
	if (!test_and_set_bit(HCLGE_STATE_RST_SERVICE_SCHED, &hdev->state))
		schedule_work(&hdev->rst_service_task);
}

static void hclge_task_schedule(struct hclge_dev *hdev)
{
	if (!test_bit(HCLGE_STATE_DOWN, &hdev->state) &&
	    !test_bit(HCLGE_STATE_REMOVING, &hdev->state) &&
	    !test_and_set_bit(HCLGE_STATE_SERVICE_SCHED, &hdev->state))
		(void)schedule_work(&hdev->service_task);
}

static int hclge_get_mac_link_status(struct hclge_dev *hdev)
{
	struct hclge_link_status_cmd *req;
	struct hclge_desc desc;
	int link_status;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_QUERY_LINK_STATUS, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "get link status cmd failed %d\n",
			ret);
		return ret;
	}

	req = (struct hclge_link_status_cmd *)desc.data;
	link_status = req->status & HCLGE_LINK_STATUS_UP_M;

	return !!link_status;
}

static int hclge_get_mac_phy_link(struct hclge_dev *hdev)
{
	int mac_state;
	int link_stat;

	if (test_bit(HCLGE_STATE_DOWN, &hdev->state))
		return 0;

	mac_state = hclge_get_mac_link_status(hdev);

	if (hdev->hw.mac.phydev) {
		if (hdev->hw.mac.phydev->state == PHY_RUNNING)
			link_stat = mac_state &
				hdev->hw.mac.phydev->link;
		else
			link_stat = 0;

	} else {
		link_stat = mac_state;
	}

	return !!link_stat;
}

static void hclge_update_link_status(struct hclge_dev *hdev)
{
	struct hnae3_client *rclient = hdev->roce_client;
	struct hnae3_client *client = hdev->nic_client;
	struct hnae3_handle *rhandle;
	struct hnae3_handle *handle;
	int state;
	int i;

	if (!client)
		return;
	state = hclge_get_mac_phy_link(hdev);
	if (state != hdev->hw.mac.link) {
		for (i = 0; i < hdev->num_vmdq_vport + 1; i++) {
			handle = &hdev->vport[i].nic;
			client->ops->link_status_change(handle, state);
			rhandle = &hdev->vport[i].roce;
			if (rclient && rclient->ops->link_status_change)
				rclient->ops->link_status_change(rhandle,
								 state);
		}
		hdev->hw.mac.link = state;
	}
}

static int hclge_get_sfp_speed(struct hclge_dev *hdev, u32 *speed)
{
	struct hclge_sfp_speed_cmd *resp = NULL;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_SFP_GET_SPEED, true);
	resp = (struct hclge_sfp_speed_cmd *)desc.data;
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret == -EOPNOTSUPP) {
		dev_warn(&hdev->pdev->dev,
			 "IMP do not support get SFP speed %d\n", ret);
		return ret;
	} else if (ret) {
		dev_err(&hdev->pdev->dev, "get sfp speed failed %d\n", ret);
		return ret;
	}

	*speed = resp->sfp_speed;

	return 0;
}

static int hclge_update_speed_duplex(struct hclge_dev *hdev)
{
	struct hclge_mac mac = hdev->hw.mac;
	int speed;
	int ret;

	/* get the speed from SFP cmd when phy
	 * doesn't exit.
	 */
	if (mac.phydev)
		return 0;

	/* if IMP does not support get SFP/qSFP speed, return directly */
	if (!hdev->support_sfp_query)
		return 0;

	ret = hclge_get_sfp_speed(hdev, &speed);
	if (ret == -EOPNOTSUPP) {
		hdev->support_sfp_query = false;
		return ret;
	} else if (ret) {
		return ret;
	}

	if (speed == HCLGE_MAC_SPEED_UNKNOWN)
		return 0; /* do nothing if no SFP */

	/* must config full duplex for SFP */
	return hclge_cfg_mac_speed_dup(hdev, speed, HCLGE_MAC_FULL);
}

static int hclge_update_speed_duplex_h(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hclge_update_speed_duplex(hdev);
}

static int hclge_get_status(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	hclge_update_link_status(hdev);

	return hdev->hw.mac.link;
}

static void hclge_service_timer(struct timer_list *t)
{
	struct hclge_dev *hdev = from_timer(hdev, t, service_timer);

	mod_timer(&hdev->service_timer, jiffies + HZ);
	hdev->hw_stats.stats_timer++;
	hclge_task_schedule(hdev);
}

static void hclge_service_complete(struct hclge_dev *hdev)
{
	WARN_ON(!test_bit(HCLGE_STATE_SERVICE_SCHED, &hdev->state));

	/* Flush memory before next watchdog */
	smp_mb__before_atomic();
	clear_bit(HCLGE_STATE_SERVICE_SCHED, &hdev->state);
}

static u32 hclge_check_event_cause(struct hclge_dev *hdev, u32 *clearval)
{
	u32 rst_src_reg, cmdq_src_reg, msix_src_reg;

	/* fetch the events from their corresponding regs */
	rst_src_reg = hclge_read_dev(&hdev->hw, HCLGE_MISC_VECTOR_INT_STS);
	cmdq_src_reg = hclge_read_dev(&hdev->hw, HCLGE_VECTOR0_CMDQ_SRC_REG);
	msix_src_reg = hclge_read_dev(&hdev->hw,
				      HCLGE_VECTOR0_PF_OTHER_INT_STS_REG);

	/* Assumption: If by any chance reset and mailbox events are reported
	 * together then we will only process reset event in this go and will
	 * defer the processing of the mailbox events. Since, we would have not
	 * cleared RX CMDQ event this time we would receive again another
	 * interrupt from H/W just for the mailbox.
	 */

	/* check for vector0 reset event sources */
	if (BIT(HCLGE_VECTOR0_IMPRESET_INT_B) & rst_src_reg) {
		dev_info(&hdev->pdev->dev, "IMP reset interrupt\n");
		set_bit(HNAE3_IMP_RESET, &hdev->reset_pending);
		set_bit(HCLGE_STATE_CMD_DISABLE, &hdev->state);
		*clearval = BIT(HCLGE_VECTOR0_IMPRESET_INT_B);
		return HCLGE_VECTOR0_EVENT_RST;
	}

	if (BIT(HCLGE_VECTOR0_GLOBALRESET_INT_B) & rst_src_reg) {
		dev_info(&hdev->pdev->dev, "global reset interrupt\n");
		set_bit(HCLGE_STATE_CMD_DISABLE, &hdev->state);
		set_bit(HNAE3_GLOBAL_RESET, &hdev->reset_pending);
		*clearval = BIT(HCLGE_VECTOR0_GLOBALRESET_INT_B);
		return HCLGE_VECTOR0_EVENT_RST;
	}

	if (BIT(HCLGE_VECTOR0_CORERESET_INT_B) & rst_src_reg) {
		dev_info(&hdev->pdev->dev, "core reset interrupt\n");
		set_bit(HCLGE_STATE_CMD_DISABLE, &hdev->state);
		set_bit(HNAE3_CORE_RESET, &hdev->reset_pending);
		*clearval = BIT(HCLGE_VECTOR0_CORERESET_INT_B);
		return HCLGE_VECTOR0_EVENT_RST;
	}

	/* check for vector0 msix event source */
	if (msix_src_reg & HCLGE_VECTOR0_REG_MSIX_MASK)
		return HCLGE_VECTOR0_EVENT_ERR;

	/* check for vector0 mailbox(=CMDQ RX) event source */
	if (BIT(HCLGE_VECTOR0_RX_CMDQ_INT_B) & cmdq_src_reg) {
		cmdq_src_reg &= ~BIT(HCLGE_VECTOR0_RX_CMDQ_INT_B);
		*clearval = cmdq_src_reg;
		return HCLGE_VECTOR0_EVENT_MBX;
	}

	return HCLGE_VECTOR0_EVENT_OTHER;
}

static void hclge_clear_event_cause(struct hclge_dev *hdev, u32 event_type,
				    u32 regclr)
{
	switch (event_type) {
	case HCLGE_VECTOR0_EVENT_RST:
		hclge_write_dev(&hdev->hw, HCLGE_MISC_RESET_STS_REG, regclr);
		break;
	case HCLGE_VECTOR0_EVENT_MBX:
		hclge_write_dev(&hdev->hw, HCLGE_VECTOR0_CMDQ_SRC_REG, regclr);
		break;
	default:
		break;
	}
}

static void hclge_clear_all_event_cause(struct hclge_dev *hdev)
{
	hclge_clear_event_cause(hdev, HCLGE_VECTOR0_EVENT_RST,
				BIT(HCLGE_VECTOR0_GLOBALRESET_INT_B) |
				BIT(HCLGE_VECTOR0_CORERESET_INT_B) |
				BIT(HCLGE_VECTOR0_IMPRESET_INT_B));
	hclge_clear_event_cause(hdev, HCLGE_VECTOR0_EVENT_MBX, 0);
}

static void hclge_enable_vector(struct hclge_misc_vector *vector, bool enable)
{
	writel(enable ? 1 : 0, vector->addr);
}

static irqreturn_t hclge_misc_irq_handle(int irq, void *data)
{
	struct hclge_dev *hdev = data;
	u32 event_cause;
	u32 clearval;

	hclge_enable_vector(&hdev->misc_vector, false);
	event_cause = hclge_check_event_cause(hdev, &clearval);

	/* vector 0 interrupt is shared with reset and mailbox source events.*/
	switch (event_cause) {
	case HCLGE_VECTOR0_EVENT_ERR:
		/* we do not know what type of reset is required now. This could
		 * only be decided after we fetch the type of errors which
		 * caused this event. Therefore, we will do below for now:
		 * 1. Assert HNAE3_UNKNOWN_RESET type of reset. This means we
		 *    have defered type of reset to be used.
		 * 2. Schedule the reset serivce task.
		 * 3. When service task receives  HNAE3_UNKNOWN_RESET type it
		 *    will fetch the correct type of reset.  This would be done
		 *    by first decoding the types of errors.
		 */
		set_bit(HNAE3_UNKNOWN_RESET, &hdev->reset_request);
		/* fall through */
	case HCLGE_VECTOR0_EVENT_RST:
		hclge_reset_task_schedule(hdev);
		break;
	case HCLGE_VECTOR0_EVENT_MBX:
		/* If we are here then,
		 * 1. Either we are not handling any mbx task and we are not
		 *    scheduled as well
		 *                        OR
		 * 2. We could be handling a mbx task but nothing more is
		 *    scheduled.
		 * In both cases, we should schedule mbx task as there are more
		 * mbx messages reported by this interrupt.
		 */
		hclge_mbx_task_schedule(hdev);
		break;
	default:
		dev_warn(&hdev->pdev->dev,
			 "received unknown or unhandled event of vector0\n");
		break;
	}

	/* clear the source of interrupt if it is not cause by reset */
	if (event_cause == HCLGE_VECTOR0_EVENT_MBX) {
		hclge_clear_event_cause(hdev, event_cause, clearval);
		hclge_enable_vector(&hdev->misc_vector, true);
	}

	return IRQ_HANDLED;
}

static void hclge_free_vector(struct hclge_dev *hdev, int vector_id)
{
	if (hdev->vector_status[vector_id] == HCLGE_INVALID_VPORT) {
		dev_warn(&hdev->pdev->dev,
			 "vector(vector_id %d) has been freed.\n", vector_id);
		return;
	}

	hdev->vector_status[vector_id] = HCLGE_INVALID_VPORT;
	hdev->num_msi_left += 1;
	hdev->num_msi_used -= 1;
}

static void hclge_get_misc_vector(struct hclge_dev *hdev)
{
	struct hclge_misc_vector *vector = &hdev->misc_vector;

	vector->vector_irq = pci_irq_vector(hdev->pdev, 0);

	vector->addr = hdev->hw.io_base + HCLGE_MISC_VECTOR_REG_BASE;
	hdev->vector_status[0] = 0;

	hdev->num_msi_left -= 1;
	hdev->num_msi_used += 1;
}

static int hclge_misc_irq_init(struct hclge_dev *hdev)
{
	int ret;

	hclge_get_misc_vector(hdev);

	/* this would be explicitly freed in the end */
	ret = request_irq(hdev->misc_vector.vector_irq, hclge_misc_irq_handle,
			  0, "hclge_misc", hdev);
	if (ret) {
		hclge_free_vector(hdev, 0);
		dev_err(&hdev->pdev->dev, "request misc irq(%d) fail\n",
			hdev->misc_vector.vector_irq);
	}

	return ret;
}

static void hclge_misc_irq_uninit(struct hclge_dev *hdev)
{
	free_irq(hdev->misc_vector.vector_irq, hdev);
	hclge_free_vector(hdev, 0);
}

int hclge_notify_client(struct hclge_dev *hdev,
			enum hnae3_reset_notify_type type)
{
	struct hnae3_client *client = hdev->nic_client;
	u16 i;

	if (!client->ops->reset_notify)
		return -EOPNOTSUPP;

	for (i = 0; i < hdev->num_vmdq_vport + 1; i++) {
		struct hnae3_handle *handle = &hdev->vport[i].nic;
		int ret;

		ret = client->ops->reset_notify(handle, type);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"notify nic client failed %d(%d)\n", type, ret);
			return ret;
		}
	}

	return 0;
}

static int hclge_notify_roce_client(struct hclge_dev *hdev,
				    enum hnae3_reset_notify_type type)
{
	struct hnae3_client *client = hdev->roce_client;
	int ret = 0;
	u16 i;

	if (!client)
		return 0;

	if (!client->ops->reset_notify)
		return -EOPNOTSUPP;

	for (i = 0; i < hdev->num_vmdq_vport + 1; i++) {
		struct hnae3_handle *handle = &hdev->vport[i].roce;

		ret = client->ops->reset_notify(handle, type);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"notify roce client failed %d(%d)",
				type, ret);
			return ret;
		}
	}

	return ret;
}

static int hclge_reset_wait(struct hclge_dev *hdev)
{
#define HCLGE_RESET_WATI_MS	100
#define HCLGE_RESET_WAIT_CNT	200
	u32 val, reg, reg_bit;
	u32 cnt = 0;

	switch (hdev->reset_type) {
	case HNAE3_IMP_RESET:
		reg = HCLGE_GLOBAL_RESET_REG;
		reg_bit = HCLGE_IMP_RESET_BIT;
		break;
	case HNAE3_GLOBAL_RESET:
		reg = HCLGE_GLOBAL_RESET_REG;
		reg_bit = HCLGE_GLOBAL_RESET_BIT;
		break;
	case HNAE3_CORE_RESET:
		reg = HCLGE_GLOBAL_RESET_REG;
		reg_bit = HCLGE_CORE_RESET_BIT;
		break;
	case HNAE3_FUNC_RESET:
		reg = HCLGE_FUN_RST_ING;
		reg_bit = HCLGE_FUN_RST_ING_B;
		break;
	case HNAE3_FLR_RESET:
		break;
	default:
		dev_err(&hdev->pdev->dev,
			"Wait for unsupported reset type: %d\n",
			hdev->reset_type);
		return -EINVAL;
	}

	if (hdev->reset_type == HNAE3_FLR_RESET) {
		while (!test_bit(HNAE3_FLR_DONE, &hdev->flr_state) &&
		       cnt++ < HCLGE_RESET_WAIT_CNT)
			msleep(HCLGE_RESET_WATI_MS);

		if (!test_bit(HNAE3_FLR_DONE, &hdev->flr_state)) {
			dev_err(&hdev->pdev->dev,
				"flr wait timeout: %d\n", cnt);
			return -EBUSY;
		}

		return 0;
	}

	val = hclge_read_dev(&hdev->hw, reg);
	while (hnae3_get_bit(val, reg_bit) && cnt < HCLGE_RESET_WAIT_CNT) {
		msleep(HCLGE_RESET_WATI_MS);
		val = hclge_read_dev(&hdev->hw, reg);
		cnt++;
	}

	if (cnt >= HCLGE_RESET_WAIT_CNT) {
		dev_warn(&hdev->pdev->dev,
			 "Wait for reset timeout: %d\n", hdev->reset_type);
		return -EBUSY;
	}

	return 0;
}

static int hclge_set_vf_rst(struct hclge_dev *hdev, int func_id, bool reset)
{
	struct hclge_vf_rst_cmd *req;
	struct hclge_desc desc;

	req = (struct hclge_vf_rst_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_GBL_RST_STATUS, false);
	req->dest_vfid = func_id;

	if (reset)
		req->vf_rst = 0x1;

	return hclge_cmd_send(&hdev->hw, &desc, 1);
}

static int hclge_set_all_vf_rst(struct hclge_dev *hdev, bool reset)
{
	int i;

	for (i = hdev->num_vmdq_vport + 1; i < hdev->num_alloc_vport; i++) {
		struct hclge_vport *vport = &hdev->vport[i];
		int ret;

		/* Send cmd to set/clear VF's FUNC_RST_ING */
		ret = hclge_set_vf_rst(hdev, vport->vport_id, reset);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"set vf(%d) rst failed %d!\n",
				vport->vport_id, ret);
			return ret;
		}

		if (!reset)
			continue;

		/* Inform VF to process the reset.
		 * hclge_inform_reset_assert_to_vf may fail if VF
		 * driver is not loaded.
		 */
		ret = hclge_inform_reset_assert_to_vf(vport);
		if (ret)
			dev_warn(&hdev->pdev->dev,
				 "inform reset to vf(%d) failed %d!\n",
				 vport->vport_id, ret);
	}

	return 0;
}

int hclge_func_reset_cmd(struct hclge_dev *hdev, int func_id)
{
	struct hclge_desc desc;
	struct hclge_reset_cmd *req = (struct hclge_reset_cmd *)desc.data;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CFG_RST_TRIGGER, false);
	hnae3_set_bit(req->mac_func_reset, HCLGE_CFG_RESET_FUNC_B, 1);
	req->fun_reset_vfid = func_id;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"send function reset cmd fail, status =%d\n", ret);

	return ret;
}

static void hclge_do_reset(struct hclge_dev *hdev)
{
	struct pci_dev *pdev = hdev->pdev;
	u32 val;

	switch (hdev->reset_type) {
	case HNAE3_GLOBAL_RESET:
		val = hclge_read_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG);
		hnae3_set_bit(val, HCLGE_GLOBAL_RESET_BIT, 1);
		hclge_write_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG, val);
		dev_info(&pdev->dev, "Global Reset requested\n");
		break;
	case HNAE3_CORE_RESET:
		val = hclge_read_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG);
		hnae3_set_bit(val, HCLGE_CORE_RESET_BIT, 1);
		hclge_write_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG, val);
		dev_info(&pdev->dev, "Core Reset requested\n");
		break;
	case HNAE3_FUNC_RESET:
		dev_info(&pdev->dev, "PF Reset requested\n");
		/* schedule again to check later */
		set_bit(HNAE3_FUNC_RESET, &hdev->reset_pending);
		hclge_reset_task_schedule(hdev);
		break;
	case HNAE3_FLR_RESET:
		dev_info(&pdev->dev, "FLR requested\n");
		/* schedule again to check later */
		set_bit(HNAE3_FLR_RESET, &hdev->reset_pending);
		hclge_reset_task_schedule(hdev);
		break;
	default:
		dev_warn(&pdev->dev,
			 "Unsupported reset type: %d\n", hdev->reset_type);
		break;
	}
}

static enum hnae3_reset_type hclge_get_reset_level(struct hclge_dev *hdev,
						   unsigned long *addr)
{
	enum hnae3_reset_type rst_level = HNAE3_NONE_RESET;

	/* first, resolve any unknown reset type to the known type(s) */
	if (test_bit(HNAE3_UNKNOWN_RESET, addr)) {
		/* we will intentionally ignore any errors from this function
		 *  as we will end up in *some* reset request in any case
		 */
		hclge_handle_hw_msix_error(hdev, addr);
		clear_bit(HNAE3_UNKNOWN_RESET, addr);
		/* We defered the clearing of the error event which caused
		 * interrupt since it was not posssible to do that in
		 * interrupt context (and this is the reason we introduced
		 * new UNKNOWN reset type). Now, the errors have been
		 * handled and cleared in hardware we can safely enable
		 * interrupts. This is an exception to the norm.
		 */
		hclge_enable_vector(&hdev->misc_vector, true);
	}

	/* return the highest priority reset level amongst all */
	if (test_bit(HNAE3_IMP_RESET, addr)) {
		rst_level = HNAE3_IMP_RESET;
		clear_bit(HNAE3_IMP_RESET, addr);
		clear_bit(HNAE3_GLOBAL_RESET, addr);
		clear_bit(HNAE3_CORE_RESET, addr);
		clear_bit(HNAE3_FUNC_RESET, addr);
	} else if (test_bit(HNAE3_GLOBAL_RESET, addr)) {
		rst_level = HNAE3_GLOBAL_RESET;
		clear_bit(HNAE3_GLOBAL_RESET, addr);
		clear_bit(HNAE3_CORE_RESET, addr);
		clear_bit(HNAE3_FUNC_RESET, addr);
	} else if (test_bit(HNAE3_CORE_RESET, addr)) {
		rst_level = HNAE3_CORE_RESET;
		clear_bit(HNAE3_CORE_RESET, addr);
		clear_bit(HNAE3_FUNC_RESET, addr);
	} else if (test_bit(HNAE3_FUNC_RESET, addr)) {
		rst_level = HNAE3_FUNC_RESET;
		clear_bit(HNAE3_FUNC_RESET, addr);
	} else if (test_bit(HNAE3_FLR_RESET, addr)) {
		rst_level = HNAE3_FLR_RESET;
		clear_bit(HNAE3_FLR_RESET, addr);
	}

	return rst_level;
}

static void hclge_clear_reset_cause(struct hclge_dev *hdev)
{
	u32 clearval = 0;

	switch (hdev->reset_type) {
	case HNAE3_IMP_RESET:
		clearval = BIT(HCLGE_VECTOR0_IMPRESET_INT_B);
		break;
	case HNAE3_GLOBAL_RESET:
		clearval = BIT(HCLGE_VECTOR0_GLOBALRESET_INT_B);
		break;
	case HNAE3_CORE_RESET:
		clearval = BIT(HCLGE_VECTOR0_CORERESET_INT_B);
		break;
	default:
		break;
	}

	if (!clearval)
		return;

	hclge_write_dev(&hdev->hw, HCLGE_MISC_RESET_STS_REG, clearval);
	hclge_enable_vector(&hdev->misc_vector, true);
}

static int hclge_reset_prepare_down(struct hclge_dev *hdev)
{
	int ret = 0;

	switch (hdev->reset_type) {
	case HNAE3_FUNC_RESET:
		/* fall through */
	case HNAE3_FLR_RESET:
		ret = hclge_set_all_vf_rst(hdev, true);
		break;
	default:
		break;
	}

	return ret;
}

static int hclge_reset_prepare_wait(struct hclge_dev *hdev)
{
	u32 reg_val;
	int ret = 0;

	switch (hdev->reset_type) {
	case HNAE3_FUNC_RESET:
		/* There is no mechanism for PF to know if VF has stopped IO
		 * for now, just wait 100 ms for VF to stop IO
		 */
		msleep(100);
		ret = hclge_func_reset_cmd(hdev, 0);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"asserting function reset fail %d!\n", ret);
			return ret;
		}

		/* After performaning pf reset, it is not necessary to do the
		 * mailbox handling or send any command to firmware, because
		 * any mailbox handling or command to firmware is only valid
		 * after hclge_cmd_init is called.
		 */
		set_bit(HCLGE_STATE_CMD_DISABLE, &hdev->state);
		break;
	case HNAE3_FLR_RESET:
		/* There is no mechanism for PF to know if VF has stopped IO
		 * for now, just wait 100 ms for VF to stop IO
		 */
		msleep(100);
		set_bit(HCLGE_STATE_CMD_DISABLE, &hdev->state);
		set_bit(HNAE3_FLR_DOWN, &hdev->flr_state);
		break;
	case HNAE3_IMP_RESET:
		reg_val = hclge_read_dev(&hdev->hw, HCLGE_PF_OTHER_INT_REG);
		hclge_write_dev(&hdev->hw, HCLGE_PF_OTHER_INT_REG,
				BIT(HCLGE_VECTOR0_IMP_RESET_INT_B) | reg_val);
		break;
	default:
		break;
	}

	dev_info(&hdev->pdev->dev, "prepare wait ok\n");

	return ret;
}

static bool hclge_reset_err_handle(struct hclge_dev *hdev, bool is_timeout)
{
#define MAX_RESET_FAIL_CNT 5
#define RESET_UPGRADE_DELAY_SEC 10

	if (hdev->reset_pending) {
		dev_info(&hdev->pdev->dev, "Reset pending %lu\n",
			 hdev->reset_pending);
		return true;
	} else if ((hdev->reset_type != HNAE3_IMP_RESET) &&
		   (hclge_read_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG) &
		    BIT(HCLGE_IMP_RESET_BIT))) {
		dev_info(&hdev->pdev->dev,
			 "reset failed because IMP Reset is pending\n");
		hclge_clear_reset_cause(hdev);
		return false;
	} else if (hdev->reset_fail_cnt < MAX_RESET_FAIL_CNT) {
		hdev->reset_fail_cnt++;
		if (is_timeout) {
			set_bit(hdev->reset_type, &hdev->reset_pending);
			dev_info(&hdev->pdev->dev,
				 "re-schedule to wait for hw reset done\n");
			return true;
		}

		dev_info(&hdev->pdev->dev, "Upgrade reset level\n");
		hclge_clear_reset_cause(hdev);
		mod_timer(&hdev->reset_timer,
			  jiffies + RESET_UPGRADE_DELAY_SEC * HZ);

		return false;
	}

	hclge_clear_reset_cause(hdev);
	dev_err(&hdev->pdev->dev, "Reset fail!\n");
	return false;
}

static int hclge_reset_prepare_up(struct hclge_dev *hdev)
{
	int ret = 0;

	switch (hdev->reset_type) {
	case HNAE3_FUNC_RESET:
		/* fall through */
	case HNAE3_FLR_RESET:
		ret = hclge_set_all_vf_rst(hdev, false);
		break;
	default:
		break;
	}

	return ret;
}

static void hclge_reset(struct hclge_dev *hdev)
{
	struct hnae3_ae_dev *ae_dev = pci_get_drvdata(hdev->pdev);
	bool is_timeout = false;
	int ret;

	/* Initialize ae_dev reset status as well, in case enet layer wants to
	 * know if device is undergoing reset
	 */
	ae_dev->reset_type = hdev->reset_type;
	hdev->reset_count++;
	/* perform reset of the stack & ae device for a client */
	ret = hclge_notify_roce_client(hdev, HNAE3_DOWN_CLIENT);
	if (ret)
		goto err_reset;

	ret = hclge_reset_prepare_down(hdev);
	if (ret)
		goto err_reset;

	rtnl_lock();
	ret = hclge_notify_client(hdev, HNAE3_DOWN_CLIENT);
	if (ret)
		goto err_reset_lock;

	rtnl_unlock();

	ret = hclge_reset_prepare_wait(hdev);
	if (ret)
		goto err_reset;

	if (hclge_reset_wait(hdev)) {
		is_timeout = true;
		goto err_reset;
	}

	ret = hclge_notify_roce_client(hdev, HNAE3_UNINIT_CLIENT);
	if (ret)
		goto err_reset;

	rtnl_lock();
	ret = hclge_notify_client(hdev, HNAE3_UNINIT_CLIENT);
	if (ret)
		goto err_reset_lock;

	ret = hclge_reset_ae_dev(hdev->ae_dev);
	if (ret)
		goto err_reset_lock;

	ret = hclge_notify_client(hdev, HNAE3_INIT_CLIENT);
	if (ret)
		goto err_reset_lock;

	ret = hclge_notify_client(hdev, HNAE3_RESTORE_CLIENT);
	if (ret)
		goto err_reset_lock;

	hclge_clear_reset_cause(hdev);

	ret = hclge_reset_prepare_up(hdev);
	if (ret)
		goto err_reset_lock;

	ret = hclge_notify_client(hdev, HNAE3_UP_CLIENT);
	if (ret)
		goto err_reset_lock;

	rtnl_unlock();

	ret = hclge_notify_roce_client(hdev, HNAE3_INIT_CLIENT);
	if (ret)
		goto err_reset;

	ret = hclge_notify_roce_client(hdev, HNAE3_UP_CLIENT);
	if (ret)
		goto err_reset;

	hdev->last_reset_time = jiffies;
	hdev->reset_fail_cnt = 0;
	ae_dev->reset_type = HNAE3_NONE_RESET;

	return;

err_reset_lock:
	rtnl_unlock();
err_reset:
	if (hclge_reset_err_handle(hdev, is_timeout))
		hclge_reset_task_schedule(hdev);
}

static void hclge_reset_event(struct pci_dev *pdev, struct hnae3_handle *handle)
{
	struct hnae3_ae_dev *ae_dev = pci_get_drvdata(pdev);
	struct hclge_dev *hdev = ae_dev->priv;

	/* We might end up getting called broadly because of 2 below cases:
	 * 1. Recoverable error was conveyed through APEI and only way to bring
	 *    normalcy is to reset.
	 * 2. A new reset request from the stack due to timeout
	 *
	 * For the first case,error event might not have ae handle available.
	 * check if this is a new reset request and we are not here just because
	 * last reset attempt did not succeed and watchdog hit us again. We will
	 * know this if last reset request did not occur very recently (watchdog
	 * timer = 5*HZ, let us check after sufficiently large time, say 4*5*Hz)
	 * In case of new request we reset the "reset level" to PF reset.
	 * And if it is a repeat reset request of the most recent one then we
	 * want to make sure we throttle the reset request. Therefore, we will
	 * not allow it again before 3*HZ times.
	 */
	if (!handle)
		handle = &hdev->vport[0].nic;

	if (time_before(jiffies, (hdev->last_reset_time + 3 * HZ)))
		return;
	else if (hdev->default_reset_request)
		hdev->reset_level =
			hclge_get_reset_level(hdev,
					      &hdev->default_reset_request);
	else if (time_after(jiffies, (hdev->last_reset_time + 4 * 5 * HZ)))
		hdev->reset_level = HNAE3_FUNC_RESET;

	dev_info(&hdev->pdev->dev, "received reset event , reset type is %d",
		 hdev->reset_level);

	/* request reset & schedule reset task */
	set_bit(hdev->reset_level, &hdev->reset_request);
	hclge_reset_task_schedule(hdev);

	if (hdev->reset_level < HNAE3_GLOBAL_RESET)
		hdev->reset_level++;
}

static void hclge_set_def_reset_request(struct hnae3_ae_dev *ae_dev,
					enum hnae3_reset_type rst_type)
{
	struct hclge_dev *hdev = ae_dev->priv;

	set_bit(rst_type, &hdev->default_reset_request);
}

static void hclge_reset_timer(struct timer_list *t)
{
	struct hclge_dev *hdev = from_timer(hdev, t, reset_timer);

	dev_info(&hdev->pdev->dev,
		 "triggering global reset in reset timer\n");
	set_bit(HNAE3_GLOBAL_RESET, &hdev->default_reset_request);
	hclge_reset_event(hdev->pdev, NULL);
}

static void hclge_reset_subtask(struct hclge_dev *hdev)
{
	/* check if there is any ongoing reset in the hardware. This status can
	 * be checked from reset_pending. If there is then, we need to wait for
	 * hardware to complete reset.
	 *    a. If we are able to figure out in reasonable time that hardware
	 *       has fully resetted then, we can proceed with driver, client
	 *       reset.
	 *    b. else, we can come back later to check this status so re-sched
	 *       now.
	 */
	hdev->last_reset_time = jiffies;
	hdev->reset_type = hclge_get_reset_level(hdev, &hdev->reset_pending);
	if (hdev->reset_type != HNAE3_NONE_RESET)
		hclge_reset(hdev);

	/* check if we got any *new* reset requests to be honored */
	hdev->reset_type = hclge_get_reset_level(hdev, &hdev->reset_request);
	if (hdev->reset_type != HNAE3_NONE_RESET)
		hclge_do_reset(hdev);

	hdev->reset_type = HNAE3_NONE_RESET;
}

static void hclge_reset_service_task(struct work_struct *work)
{
	struct hclge_dev *hdev =
		container_of(work, struct hclge_dev, rst_service_task);

	if (test_and_set_bit(HCLGE_STATE_RST_HANDLING, &hdev->state))
		return;

	clear_bit(HCLGE_STATE_RST_SERVICE_SCHED, &hdev->state);

	hclge_reset_subtask(hdev);

	clear_bit(HCLGE_STATE_RST_HANDLING, &hdev->state);
}

static void hclge_mailbox_service_task(struct work_struct *work)
{
	struct hclge_dev *hdev =
		container_of(work, struct hclge_dev, mbx_service_task);

	if (test_and_set_bit(HCLGE_STATE_MBX_HANDLING, &hdev->state))
		return;

	clear_bit(HCLGE_STATE_MBX_SERVICE_SCHED, &hdev->state);

	hclge_mbx_handler(hdev);

	clear_bit(HCLGE_STATE_MBX_HANDLING, &hdev->state);
}

static void hclge_update_vport_alive(struct hclge_dev *hdev)
{
	int i;

	/* start from vport 1 for PF is always alive */
	for (i = 1; i < hdev->num_alloc_vport; i++) {
		struct hclge_vport *vport = &hdev->vport[i];

		if (time_after(jiffies, vport->last_active_jiffies + 8 * HZ))
			clear_bit(HCLGE_VPORT_STATE_ALIVE, &vport->state);

		/* If vf is not alive, set to default value */
		if (!test_bit(HCLGE_VPORT_STATE_ALIVE, &vport->state))
			vport->mps = HCLGE_MAC_DEFAULT_FRAME;
	}
}

static void hclge_service_task(struct work_struct *work)
{
	struct hclge_dev *hdev =
		container_of(work, struct hclge_dev, service_task);

	if (hdev->hw_stats.stats_timer >= HCLGE_STATS_TIMER_INTERVAL) {
		hclge_update_stats_for_all(hdev);
		hdev->hw_stats.stats_timer = 0;
	}

	hclge_update_speed_duplex(hdev);
	hclge_update_link_status(hdev);
	hclge_update_vport_alive(hdev);
	hclge_service_complete(hdev);
}

struct hclge_vport *hclge_get_vport(struct hnae3_handle *handle)
{
	/* VF handle has no client */
	if (!handle->client)
		return container_of(handle, struct hclge_vport, nic);
	else if (handle->client->type == HNAE3_CLIENT_ROCE)
		return container_of(handle, struct hclge_vport, roce);
	else
		return container_of(handle, struct hclge_vport, nic);
}

static int hclge_get_vector(struct hnae3_handle *handle, u16 vector_num,
			    struct hnae3_vector_info *vector_info)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hnae3_vector_info *vector = vector_info;
	struct hclge_dev *hdev = vport->back;
	int alloc = 0;
	int i, j;

	vector_num = min(hdev->num_msi_left, vector_num);

	for (j = 0; j < vector_num; j++) {
		for (i = 1; i < hdev->num_msi; i++) {
			if (hdev->vector_status[i] == HCLGE_INVALID_VPORT) {
				vector->vector = pci_irq_vector(hdev->pdev, i);
				vector->io_addr = hdev->hw.io_base +
					HCLGE_VECTOR_REG_BASE +
					(i - 1) * HCLGE_VECTOR_REG_OFFSET +
					vport->vport_id *
					HCLGE_VECTOR_VF_OFFSET;
				hdev->vector_status[i] = vport->vport_id;
				hdev->vector_irq[i] = vector->vector;

				vector++;
				alloc++;

				break;
			}
		}
	}
	hdev->num_msi_left -= alloc;
	hdev->num_msi_used += alloc;

	return alloc;
}

static int hclge_get_vector_index(struct hclge_dev *hdev, int vector)
{
	int i;

	for (i = 0; i < hdev->num_msi; i++)
		if (vector == hdev->vector_irq[i])
			return i;

	return -EINVAL;
}

static int hclge_put_vector(struct hnae3_handle *handle, int vector)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int vector_id;

	vector_id = hclge_get_vector_index(hdev, vector);
	if (vector_id < 0) {
		dev_err(&hdev->pdev->dev,
			"Get vector index fail. vector_id =%d\n", vector_id);
		return vector_id;
	}

	hclge_free_vector(hdev, vector_id);

	return 0;
}

static u32 hclge_get_rss_key_size(struct hnae3_handle *handle)
{
	return HCLGE_RSS_KEY_SIZE;
}

static u32 hclge_get_rss_indir_size(struct hnae3_handle *handle)
{
	return HCLGE_RSS_IND_TBL_SIZE;
}

static int hclge_set_rss_algo_key(struct hclge_dev *hdev,
				  const u8 hfunc, const u8 *key)
{
	struct hclge_rss_config_cmd *req;
	struct hclge_desc desc;
	int key_offset;
	int key_size;
	int ret;

	req = (struct hclge_rss_config_cmd *)desc.data;

	for (key_offset = 0; key_offset < 3; key_offset++) {
		hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RSS_GENERIC_CONFIG,
					   false);

		req->hash_config |= (hfunc & HCLGE_RSS_HASH_ALGO_MASK);
		req->hash_config |= (key_offset << HCLGE_RSS_HASH_KEY_OFFSET_B);

		if (key_offset == 2)
			key_size =
			HCLGE_RSS_KEY_SIZE - HCLGE_RSS_HASH_KEY_NUM * 2;
		else
			key_size = HCLGE_RSS_HASH_KEY_NUM;

		memcpy(req->hash_key,
		       key + key_offset * HCLGE_RSS_HASH_KEY_NUM, key_size);

		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"Configure RSS config fail, status = %d\n",
				ret);
			return ret;
		}
	}
	return 0;
}

static int hclge_set_rss_indir_table(struct hclge_dev *hdev, const u8 *indir)
{
	struct hclge_rss_indirection_table_cmd *req;
	struct hclge_desc desc;
	int i, j;
	int ret;

	req = (struct hclge_rss_indirection_table_cmd *)desc.data;

	for (i = 0; i < HCLGE_RSS_CFG_TBL_NUM; i++) {
		hclge_cmd_setup_basic_desc
			(&desc, HCLGE_OPC_RSS_INDIR_TABLE, false);

		req->start_table_index =
			cpu_to_le16(i * HCLGE_RSS_CFG_TBL_SIZE);
		req->rss_set_bitmap = cpu_to_le16(HCLGE_RSS_SET_BITMAP_MSK);

		for (j = 0; j < HCLGE_RSS_CFG_TBL_SIZE; j++)
			req->rss_result[j] =
				indir[i * HCLGE_RSS_CFG_TBL_SIZE + j];

		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"Configure rss indir table fail,status = %d\n",
				ret);
			return ret;
		}
	}
	return 0;
}

static int hclge_set_rss_tc_mode(struct hclge_dev *hdev, u16 *tc_valid,
				 u16 *tc_size, u16 *tc_offset)
{
	struct hclge_rss_tc_mode_cmd *req;
	struct hclge_desc desc;
	int ret;
	int i;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RSS_TC_MODE, false);
	req = (struct hclge_rss_tc_mode_cmd *)desc.data;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		u16 mode = 0;

		hnae3_set_bit(mode, HCLGE_RSS_TC_VALID_B, (tc_valid[i] & 0x1));
		hnae3_set_field(mode, HCLGE_RSS_TC_SIZE_M,
				HCLGE_RSS_TC_SIZE_S, tc_size[i]);
		hnae3_set_field(mode, HCLGE_RSS_TC_OFFSET_M,
				HCLGE_RSS_TC_OFFSET_S, tc_offset[i]);

		req->rss_tc_mode[i] = cpu_to_le16(mode);
	}

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Configure rss tc mode fail, status = %d\n", ret);

	return ret;
}

static void hclge_get_rss_type(struct hclge_vport *vport)
{
	if (vport->rss_tuple_sets.ipv4_tcp_en ||
	    vport->rss_tuple_sets.ipv4_udp_en ||
	    vport->rss_tuple_sets.ipv4_sctp_en ||
	    vport->rss_tuple_sets.ipv6_tcp_en ||
	    vport->rss_tuple_sets.ipv6_udp_en ||
	    vport->rss_tuple_sets.ipv6_sctp_en)
		vport->nic.kinfo.rss_type = PKT_HASH_TYPE_L4;
	else if (vport->rss_tuple_sets.ipv4_fragment_en ||
		 vport->rss_tuple_sets.ipv6_fragment_en)
		vport->nic.kinfo.rss_type = PKT_HASH_TYPE_L3;
	else
		vport->nic.kinfo.rss_type = PKT_HASH_TYPE_NONE;
}

static int hclge_set_rss_input_tuple(struct hclge_dev *hdev)
{
	struct hclge_rss_input_tuple_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RSS_INPUT_TUPLE, false);

	req = (struct hclge_rss_input_tuple_cmd *)desc.data;

	/* Get the tuple cfg from pf */
	req->ipv4_tcp_en = hdev->vport[0].rss_tuple_sets.ipv4_tcp_en;
	req->ipv4_udp_en = hdev->vport[0].rss_tuple_sets.ipv4_udp_en;
	req->ipv4_sctp_en = hdev->vport[0].rss_tuple_sets.ipv4_sctp_en;
	req->ipv4_fragment_en = hdev->vport[0].rss_tuple_sets.ipv4_fragment_en;
	req->ipv6_tcp_en = hdev->vport[0].rss_tuple_sets.ipv6_tcp_en;
	req->ipv6_udp_en = hdev->vport[0].rss_tuple_sets.ipv6_udp_en;
	req->ipv6_sctp_en = hdev->vport[0].rss_tuple_sets.ipv6_sctp_en;
	req->ipv6_fragment_en = hdev->vport[0].rss_tuple_sets.ipv6_fragment_en;
	hclge_get_rss_type(&hdev->vport[0]);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Configure rss input fail, status = %d\n", ret);
	return ret;
}

static int hclge_get_rss(struct hnae3_handle *handle, u32 *indir,
			 u8 *key, u8 *hfunc)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	int i;

	/* Get hash algorithm */
	if (hfunc) {
		switch (vport->rss_algo) {
		case HCLGE_RSS_HASH_ALGO_TOEPLITZ:
			*hfunc = ETH_RSS_HASH_TOP;
			break;
		case HCLGE_RSS_HASH_ALGO_SIMPLE:
			*hfunc = ETH_RSS_HASH_XOR;
			break;
		default:
			*hfunc = ETH_RSS_HASH_UNKNOWN;
			break;
		}
	}

	/* Get the RSS Key required by the user */
	if (key)
		memcpy(key, vport->rss_hash_key, HCLGE_RSS_KEY_SIZE);

	/* Get indirect table */
	if (indir)
		for (i = 0; i < HCLGE_RSS_IND_TBL_SIZE; i++)
			indir[i] =  vport->rss_indirection_tbl[i];

	return 0;
}

static int hclge_set_rss(struct hnae3_handle *handle, const u32 *indir,
			 const  u8 *key, const  u8 hfunc)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	u8 hash_algo;
	int ret, i;

	/* Set the RSS Hash Key if specififed by the user */
	if (key) {
		switch (hfunc) {
		case ETH_RSS_HASH_TOP:
			hash_algo = HCLGE_RSS_HASH_ALGO_TOEPLITZ;
			break;
		case ETH_RSS_HASH_XOR:
			hash_algo = HCLGE_RSS_HASH_ALGO_SIMPLE;
			break;
		case ETH_RSS_HASH_NO_CHANGE:
			hash_algo = vport->rss_algo;
			break;
		default:
			return -EINVAL;
		}

		ret = hclge_set_rss_algo_key(hdev, hash_algo, key);
		if (ret)
			return ret;

		/* Update the shadow RSS key with user specified qids */
		memcpy(vport->rss_hash_key, key, HCLGE_RSS_KEY_SIZE);
		vport->rss_algo = hash_algo;
	}

	/* Update the shadow RSS table with user specified qids */
	for (i = 0; i < HCLGE_RSS_IND_TBL_SIZE; i++)
		vport->rss_indirection_tbl[i] = indir[i];

	/* Update the hardware */
	return hclge_set_rss_indir_table(hdev, vport->rss_indirection_tbl);
}

static u8 hclge_get_rss_hash_bits(struct ethtool_rxnfc *nfc)
{
	u8 hash_sets = nfc->data & RXH_L4_B_0_1 ? HCLGE_S_PORT_BIT : 0;

	if (nfc->data & RXH_L4_B_2_3)
		hash_sets |= HCLGE_D_PORT_BIT;
	else
		hash_sets &= ~HCLGE_D_PORT_BIT;

	if (nfc->data & RXH_IP_SRC)
		hash_sets |= HCLGE_S_IP_BIT;
	else
		hash_sets &= ~HCLGE_S_IP_BIT;

	if (nfc->data & RXH_IP_DST)
		hash_sets |= HCLGE_D_IP_BIT;
	else
		hash_sets &= ~HCLGE_D_IP_BIT;

	if (nfc->flow_type == SCTP_V4_FLOW || nfc->flow_type == SCTP_V6_FLOW)
		hash_sets |= HCLGE_V_TAG_BIT;

	return hash_sets;
}

static int hclge_set_rss_tuple(struct hnae3_handle *handle,
			       struct ethtool_rxnfc *nfc)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hclge_rss_input_tuple_cmd *req;
	struct hclge_desc desc;
	u8 tuple_sets;
	int ret;

	if (nfc->data & ~(RXH_IP_SRC | RXH_IP_DST |
			  RXH_L4_B_0_1 | RXH_L4_B_2_3))
		return -EINVAL;

	req = (struct hclge_rss_input_tuple_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RSS_INPUT_TUPLE, false);

	req->ipv4_tcp_en = vport->rss_tuple_sets.ipv4_tcp_en;
	req->ipv4_udp_en = vport->rss_tuple_sets.ipv4_udp_en;
	req->ipv4_sctp_en = vport->rss_tuple_sets.ipv4_sctp_en;
	req->ipv4_fragment_en = vport->rss_tuple_sets.ipv4_fragment_en;
	req->ipv6_tcp_en = vport->rss_tuple_sets.ipv6_tcp_en;
	req->ipv6_udp_en = vport->rss_tuple_sets.ipv6_udp_en;
	req->ipv6_sctp_en = vport->rss_tuple_sets.ipv6_sctp_en;
	req->ipv6_fragment_en = vport->rss_tuple_sets.ipv6_fragment_en;

	tuple_sets = hclge_get_rss_hash_bits(nfc);
	switch (nfc->flow_type) {
	case TCP_V4_FLOW:
		req->ipv4_tcp_en = tuple_sets;
		break;
	case TCP_V6_FLOW:
		req->ipv6_tcp_en = tuple_sets;
		break;
	case UDP_V4_FLOW:
		req->ipv4_udp_en = tuple_sets;
		break;
	case UDP_V6_FLOW:
		req->ipv6_udp_en = tuple_sets;
		break;
	case SCTP_V4_FLOW:
		req->ipv4_sctp_en = tuple_sets;
		break;
	case SCTP_V6_FLOW:
		if ((nfc->data & RXH_L4_B_0_1) ||
		    (nfc->data & RXH_L4_B_2_3))
			return -EINVAL;

		req->ipv6_sctp_en = tuple_sets;
		break;
	case IPV4_FLOW:
		req->ipv4_fragment_en = HCLGE_RSS_INPUT_TUPLE_OTHER;
		break;
	case IPV6_FLOW:
		req->ipv6_fragment_en = HCLGE_RSS_INPUT_TUPLE_OTHER;
		break;
	default:
		return -EINVAL;
	}

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Set rss tuple fail, status = %d\n", ret);
		return ret;
	}

	vport->rss_tuple_sets.ipv4_tcp_en = req->ipv4_tcp_en;
	vport->rss_tuple_sets.ipv4_udp_en = req->ipv4_udp_en;
	vport->rss_tuple_sets.ipv4_sctp_en = req->ipv4_sctp_en;
	vport->rss_tuple_sets.ipv4_fragment_en = req->ipv4_fragment_en;
	vport->rss_tuple_sets.ipv6_tcp_en = req->ipv6_tcp_en;
	vport->rss_tuple_sets.ipv6_udp_en = req->ipv6_udp_en;
	vport->rss_tuple_sets.ipv6_sctp_en = req->ipv6_sctp_en;
	vport->rss_tuple_sets.ipv6_fragment_en = req->ipv6_fragment_en;
	hclge_get_rss_type(vport);
	return 0;
}

static int hclge_get_rss_tuple(struct hnae3_handle *handle,
			       struct ethtool_rxnfc *nfc)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	u8 tuple_sets;

	nfc->data = 0;

	switch (nfc->flow_type) {
	case TCP_V4_FLOW:
		tuple_sets = vport->rss_tuple_sets.ipv4_tcp_en;
		break;
	case UDP_V4_FLOW:
		tuple_sets = vport->rss_tuple_sets.ipv4_udp_en;
		break;
	case TCP_V6_FLOW:
		tuple_sets = vport->rss_tuple_sets.ipv6_tcp_en;
		break;
	case UDP_V6_FLOW:
		tuple_sets = vport->rss_tuple_sets.ipv6_udp_en;
		break;
	case SCTP_V4_FLOW:
		tuple_sets = vport->rss_tuple_sets.ipv4_sctp_en;
		break;
	case SCTP_V6_FLOW:
		tuple_sets = vport->rss_tuple_sets.ipv6_sctp_en;
		break;
	case IPV4_FLOW:
	case IPV6_FLOW:
		tuple_sets = HCLGE_S_IP_BIT | HCLGE_D_IP_BIT;
		break;
	default:
		return -EINVAL;
	}

	if (!tuple_sets)
		return 0;

	if (tuple_sets & HCLGE_D_PORT_BIT)
		nfc->data |= RXH_L4_B_2_3;
	if (tuple_sets & HCLGE_S_PORT_BIT)
		nfc->data |= RXH_L4_B_0_1;
	if (tuple_sets & HCLGE_D_IP_BIT)
		nfc->data |= RXH_IP_DST;
	if (tuple_sets & HCLGE_S_IP_BIT)
		nfc->data |= RXH_IP_SRC;

	return 0;
}

static int hclge_get_tc_size(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hdev->rss_size_max;
}

int hclge_rss_init_hw(struct hclge_dev *hdev)
{
	struct hclge_vport *vport = hdev->vport;
	u8 *rss_indir = vport[0].rss_indirection_tbl;
	u16 rss_size = vport[0].alloc_rss_size;
	u8 *key = vport[0].rss_hash_key;
	u8 hfunc = vport[0].rss_algo;
	u16 tc_offset[HCLGE_MAX_TC_NUM];
	u16 tc_valid[HCLGE_MAX_TC_NUM];
	u16 tc_size[HCLGE_MAX_TC_NUM];
	u16 roundup_size;
	int i, ret;

	ret = hclge_set_rss_indir_table(hdev, rss_indir);
	if (ret)
		return ret;

	ret = hclge_set_rss_algo_key(hdev, hfunc, key);
	if (ret)
		return ret;

	ret = hclge_set_rss_input_tuple(hdev);
	if (ret)
		return ret;

	/* Each TC have the same queue size, and tc_size set to hardware is
	 * the log2 of roundup power of two of rss_size, the acutal queue
	 * size is limited by indirection table.
	 */
	if (rss_size > HCLGE_RSS_TC_SIZE_7 || rss_size == 0) {
		dev_err(&hdev->pdev->dev,
			"Configure rss tc size failed, invalid TC_SIZE = %d\n",
			rss_size);
		return -EINVAL;
	}

	roundup_size = roundup_pow_of_two(rss_size);
	roundup_size = ilog2(roundup_size);

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		tc_valid[i] = 0;

		if (!(hdev->hw_tc_map & BIT(i)))
			continue;

		tc_valid[i] = 1;
		tc_size[i] = roundup_size;
		tc_offset[i] = rss_size * i;
	}

	return hclge_set_rss_tc_mode(hdev, tc_valid, tc_size, tc_offset);
}

void hclge_rss_indir_init_cfg(struct hclge_dev *hdev)
{
	struct hclge_vport *vport = hdev->vport;
	int i, j;

	for (j = 0; j < hdev->num_vmdq_vport + 1; j++) {
		for (i = 0; i < HCLGE_RSS_IND_TBL_SIZE; i++)
			vport[j].rss_indirection_tbl[i] =
				i % vport[j].alloc_rss_size;
	}
}

static void hclge_rss_init_cfg(struct hclge_dev *hdev)
{
	int i, rss_algo = HCLGE_RSS_HASH_ALGO_TOEPLITZ;
	struct hclge_vport *vport = hdev->vport;

	if (hdev->pdev->revision >= 0x21)
		rss_algo = HCLGE_RSS_HASH_ALGO_SIMPLE;

	for (i = 0; i < hdev->num_vmdq_vport + 1; i++) {
		vport[i].rss_tuple_sets.ipv4_tcp_en =
			HCLGE_RSS_INPUT_TUPLE_OTHER;
		vport[i].rss_tuple_sets.ipv4_udp_en =
			HCLGE_RSS_INPUT_TUPLE_OTHER;
		vport[i].rss_tuple_sets.ipv4_sctp_en =
			HCLGE_RSS_INPUT_TUPLE_SCTP;
		vport[i].rss_tuple_sets.ipv4_fragment_en =
			HCLGE_RSS_INPUT_TUPLE_OTHER;
		vport[i].rss_tuple_sets.ipv6_tcp_en =
			HCLGE_RSS_INPUT_TUPLE_OTHER;
		vport[i].rss_tuple_sets.ipv6_udp_en =
			HCLGE_RSS_INPUT_TUPLE_OTHER;
		vport[i].rss_tuple_sets.ipv6_sctp_en =
			HCLGE_RSS_INPUT_TUPLE_SCTP;
		vport[i].rss_tuple_sets.ipv6_fragment_en =
			HCLGE_RSS_INPUT_TUPLE_OTHER;

		vport[i].rss_algo = rss_algo;

		memcpy(vport[i].rss_hash_key, hclge_hash_key,
		       HCLGE_RSS_KEY_SIZE);
	}

	hclge_rss_indir_init_cfg(hdev);
}

int hclge_bind_ring_with_vector(struct hclge_vport *vport,
				int vector_id, bool en,
				struct hnae3_ring_chain_node *ring_chain)
{
	struct hclge_dev *hdev = vport->back;
	struct hnae3_ring_chain_node *node;
	struct hclge_desc desc;
	struct hclge_ctrl_vector_chain_cmd *req
		= (struct hclge_ctrl_vector_chain_cmd *)desc.data;
	enum hclge_cmd_status status;
	enum hclge_opcode_type op;
	u16 tqp_type_and_id;
	int i;

	op = en ? HCLGE_OPC_ADD_RING_TO_VECTOR : HCLGE_OPC_DEL_RING_TO_VECTOR;
	hclge_cmd_setup_basic_desc(&desc, op, false);
	req->int_vector_id = vector_id;

	i = 0;
	for (node = ring_chain; node; node = node->next) {
		tqp_type_and_id = le16_to_cpu(req->tqp_type_and_id[i]);
		hnae3_set_field(tqp_type_and_id,  HCLGE_INT_TYPE_M,
				HCLGE_INT_TYPE_S,
				hnae3_get_bit(node->flag, HNAE3_RING_TYPE_B));
		hnae3_set_field(tqp_type_and_id, HCLGE_TQP_ID_M,
				HCLGE_TQP_ID_S, node->tqp_index);
		hnae3_set_field(tqp_type_and_id, HCLGE_INT_GL_IDX_M,
				HCLGE_INT_GL_IDX_S,
				hnae3_get_field(node->int_gl_idx,
						HNAE3_RING_GL_IDX_M,
						HNAE3_RING_GL_IDX_S));
		req->tqp_type_and_id[i] = cpu_to_le16(tqp_type_and_id);
		if (++i >= HCLGE_VECTOR_ELEMENTS_PER_CMD) {
			req->int_cause_num = HCLGE_VECTOR_ELEMENTS_PER_CMD;
			req->vfid = vport->vport_id;

			status = hclge_cmd_send(&hdev->hw, &desc, 1);
			if (status) {
				dev_err(&hdev->pdev->dev,
					"Map TQP fail, status is %d.\n",
					status);
				return -EIO;
			}
			i = 0;

			hclge_cmd_setup_basic_desc(&desc,
						   op,
						   false);
			req->int_vector_id = vector_id;
		}
	}

	if (i > 0) {
		req->int_cause_num = i;
		req->vfid = vport->vport_id;
		status = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (status) {
			dev_err(&hdev->pdev->dev,
				"Map TQP fail, status is %d.\n", status);
			return -EIO;
		}
	}

	return 0;
}

static int hclge_map_ring_to_vector(struct hnae3_handle *handle,
				    int vector,
				    struct hnae3_ring_chain_node *ring_chain)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int vector_id;

	vector_id = hclge_get_vector_index(hdev, vector);
	if (vector_id < 0) {
		dev_err(&hdev->pdev->dev,
			"Get vector index fail. vector_id =%d\n", vector_id);
		return vector_id;
	}

	return hclge_bind_ring_with_vector(vport, vector_id, true, ring_chain);
}

static int hclge_unmap_ring_frm_vector(struct hnae3_handle *handle,
				       int vector,
				       struct hnae3_ring_chain_node *ring_chain)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int vector_id, ret;

	if (test_bit(HCLGE_STATE_RST_HANDLING, &hdev->state))
		return 0;

	vector_id = hclge_get_vector_index(hdev, vector);
	if (vector_id < 0) {
		dev_err(&handle->pdev->dev,
			"Get vector index fail. ret =%d\n", vector_id);
		return vector_id;
	}

	ret = hclge_bind_ring_with_vector(vport, vector_id, false, ring_chain);
	if (ret)
		dev_err(&handle->pdev->dev,
			"Unmap ring from vector fail. vectorid=%d, ret =%d\n",
			vector_id,
			ret);

	return ret;
}

int hclge_cmd_set_promisc_mode(struct hclge_dev *hdev,
			       struct hclge_promisc_param *param)
{
	struct hclge_promisc_cfg_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CFG_PROMISC_MODE, false);

	req = (struct hclge_promisc_cfg_cmd *)desc.data;
	req->vf_id = param->vf_id;

	/* HCLGE_PROMISC_TX_EN_B and HCLGE_PROMISC_RX_EN_B are not supported on
	 * pdev revision(0x20), new revision support them. The
	 * value of this two fields will not return error when driver
	 * send command to fireware in revision(0x20).
	 */
	req->flag = (param->enable << HCLGE_PROMISC_EN_B) |
		HCLGE_PROMISC_TX_EN_B | HCLGE_PROMISC_RX_EN_B;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Set promisc mode fail, status is %d.\n", ret);

	return ret;
}

void hclge_promisc_param_init(struct hclge_promisc_param *param, bool en_uc,
			      bool en_mc, bool en_bc, int vport_id)
{
	if (!param)
		return;

	memset(param, 0, sizeof(struct hclge_promisc_param));
	if (en_uc)
		param->enable = HCLGE_PROMISC_EN_UC;
	if (en_mc)
		param->enable |= HCLGE_PROMISC_EN_MC;
	if (en_bc)
		param->enable |= HCLGE_PROMISC_EN_BC;
	param->vf_id = vport_id;
}

static int hclge_set_promisc_mode(struct hnae3_handle *handle, bool en_uc_pmc,
				  bool en_mc_pmc)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hclge_promisc_param param;
	bool en_bc_pmc = true;

	/* For revision 0x20, if broadcast promisc enabled, vlan filter is
	 * always bypassed. So broadcast promisc should be disabled until
	 * user enable promisc mode
	 */
	if (handle->pdev->revision == 0x20)
		en_bc_pmc = handle->netdev_flags & HNAE3_BPE ? true : false;

	hclge_promisc_param_init(&param, en_uc_pmc, en_mc_pmc, en_bc_pmc,
				 vport->vport_id);
	return hclge_cmd_set_promisc_mode(hdev, &param);
}

static int hclge_get_fd_mode(struct hclge_dev *hdev, u8 *fd_mode)
{
	struct hclge_get_fd_mode_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_FD_MODE_CTRL, true);

	req = (struct hclge_get_fd_mode_cmd *)desc.data;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "get fd mode fail, ret=%d\n", ret);
		return ret;
	}

	*fd_mode = req->mode;

	return ret;
}

static int hclge_get_fd_allocation(struct hclge_dev *hdev,
				   u32 *stage1_entry_num,
				   u32 *stage2_entry_num,
				   u16 *stage1_counter_num,
				   u16 *stage2_counter_num)
{
	struct hclge_get_fd_allocation_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_FD_GET_ALLOCATION, true);

	req = (struct hclge_get_fd_allocation_cmd *)desc.data;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "query fd allocation fail, ret=%d\n",
			ret);
		return ret;
	}

	*stage1_entry_num = le32_to_cpu(req->stage1_entry_num);
	*stage2_entry_num = le32_to_cpu(req->stage2_entry_num);
	*stage1_counter_num = le16_to_cpu(req->stage1_counter_num);
	*stage2_counter_num = le16_to_cpu(req->stage2_counter_num);

	return ret;
}

static int hclge_set_fd_key_config(struct hclge_dev *hdev, int stage_num)
{
	struct hclge_set_fd_key_config_cmd *req;
	struct hclge_fd_key_cfg *stage;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_FD_KEY_CONFIG, false);

	req = (struct hclge_set_fd_key_config_cmd *)desc.data;
	stage = &hdev->fd_cfg.key_cfg[stage_num];
	req->stage = stage_num;
	req->key_select = stage->key_sel;
	req->inner_sipv6_word_en = stage->inner_sipv6_word_en;
	req->inner_dipv6_word_en = stage->inner_dipv6_word_en;
	req->outer_sipv6_word_en = stage->outer_sipv6_word_en;
	req->outer_dipv6_word_en = stage->outer_dipv6_word_en;
	req->tuple_mask = cpu_to_le32(~stage->tuple_active);
	req->meta_data_mask = cpu_to_le32(~stage->meta_data_active);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev, "set fd key fail, ret=%d\n", ret);

	return ret;
}

static int hclge_init_fd_config(struct hclge_dev *hdev)
{
#define LOW_2_WORDS		0x03
	struct hclge_fd_key_cfg *key_cfg;
	int ret;

	if (!hnae3_dev_fd_supported(hdev))
		return 0;

	ret = hclge_get_fd_mode(hdev, &hdev->fd_cfg.fd_mode);
	if (ret)
		return ret;

	switch (hdev->fd_cfg.fd_mode) {
	case HCLGE_FD_MODE_DEPTH_2K_WIDTH_400B_STAGE_1:
		hdev->fd_cfg.max_key_length = MAX_KEY_LENGTH;
		break;
	case HCLGE_FD_MODE_DEPTH_4K_WIDTH_200B_STAGE_1:
		hdev->fd_cfg.max_key_length = MAX_KEY_LENGTH / 2;
		break;
	default:
		dev_err(&hdev->pdev->dev,
			"Unsupported flow director mode %d\n",
			hdev->fd_cfg.fd_mode);
		return -EOPNOTSUPP;
	}

	hdev->fd_cfg.proto_support =
		TCP_V4_FLOW | UDP_V4_FLOW | SCTP_V4_FLOW | TCP_V6_FLOW |
		UDP_V6_FLOW | SCTP_V6_FLOW | IPV4_USER_FLOW | IPV6_USER_FLOW;
	key_cfg = &hdev->fd_cfg.key_cfg[HCLGE_FD_STAGE_1];
	key_cfg->key_sel = HCLGE_FD_KEY_BASE_ON_TUPLE,
	key_cfg->inner_sipv6_word_en = LOW_2_WORDS;
	key_cfg->inner_dipv6_word_en = LOW_2_WORDS;
	key_cfg->outer_sipv6_word_en = 0;
	key_cfg->outer_dipv6_word_en = 0;

	key_cfg->tuple_active = BIT(INNER_VLAN_TAG_FST) | BIT(INNER_ETH_TYPE) |
				BIT(INNER_IP_PROTO) | BIT(INNER_IP_TOS) |
				BIT(INNER_SRC_IP) | BIT(INNER_DST_IP) |
				BIT(INNER_SRC_PORT) | BIT(INNER_DST_PORT);

	/* If use max 400bit key, we can support tuples for ether type */
	if (hdev->fd_cfg.max_key_length == MAX_KEY_LENGTH) {
		hdev->fd_cfg.proto_support |= ETHER_FLOW;
		key_cfg->tuple_active |=
				BIT(INNER_DST_MAC) | BIT(INNER_SRC_MAC);
	}

	/* roce_type is used to filter roce frames
	 * dst_vport is used to specify the rule
	 */
	key_cfg->meta_data_active = BIT(ROCE_TYPE) | BIT(DST_VPORT);

	ret = hclge_get_fd_allocation(hdev,
				      &hdev->fd_cfg.rule_num[HCLGE_FD_STAGE_1],
				      &hdev->fd_cfg.rule_num[HCLGE_FD_STAGE_2],
				      &hdev->fd_cfg.cnt_num[HCLGE_FD_STAGE_1],
				      &hdev->fd_cfg.cnt_num[HCLGE_FD_STAGE_2]);
	if (ret)
		return ret;

	return hclge_set_fd_key_config(hdev, HCLGE_FD_STAGE_1);
}

static int hclge_fd_tcam_config(struct hclge_dev *hdev, u8 stage, bool sel_x,
				int loc, u8 *key, bool is_add)
{
	struct hclge_fd_tcam_config_1_cmd *req1;
	struct hclge_fd_tcam_config_2_cmd *req2;
	struct hclge_fd_tcam_config_3_cmd *req3;
	struct hclge_desc desc[3];
	int ret;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_FD_TCAM_OP, false);
	desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[1], HCLGE_OPC_FD_TCAM_OP, false);
	desc[1].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[2], HCLGE_OPC_FD_TCAM_OP, false);

	req1 = (struct hclge_fd_tcam_config_1_cmd *)desc[0].data;
	req2 = (struct hclge_fd_tcam_config_2_cmd *)desc[1].data;
	req3 = (struct hclge_fd_tcam_config_3_cmd *)desc[2].data;

	req1->stage = stage;
	req1->xy_sel = sel_x ? 1 : 0;
	hnae3_set_bit(req1->port_info, HCLGE_FD_EPORT_SW_EN_B, 0);
	req1->index = cpu_to_le32(loc);
	req1->entry_vld = sel_x ? is_add : 0;

	if (key) {
		memcpy(req1->tcam_data, &key[0], sizeof(req1->tcam_data));
		memcpy(req2->tcam_data, &key[sizeof(req1->tcam_data)],
		       sizeof(req2->tcam_data));
		memcpy(req3->tcam_data, &key[sizeof(req1->tcam_data) +
		       sizeof(req2->tcam_data)], sizeof(req3->tcam_data));
	}

	ret = hclge_cmd_send(&hdev->hw, desc, 3);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"config tcam key fail, ret=%d\n",
			ret);

	return ret;
}

static int hclge_fd_ad_config(struct hclge_dev *hdev, u8 stage, int loc,
			      struct hclge_fd_ad_data *action)
{
	struct hclge_fd_ad_config_cmd *req;
	struct hclge_desc desc;
	u64 ad_data = 0;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_FD_AD_OP, false);

	req = (struct hclge_fd_ad_config_cmd *)desc.data;
	req->index = cpu_to_le32(loc);
	req->stage = stage;

	hnae3_set_bit(ad_data, HCLGE_FD_AD_WR_RULE_ID_B,
		      action->write_rule_id_to_bd);
	hnae3_set_field(ad_data, HCLGE_FD_AD_RULE_ID_M, HCLGE_FD_AD_RULE_ID_S,
			action->rule_id);
	ad_data <<= 32;
	hnae3_set_bit(ad_data, HCLGE_FD_AD_DROP_B, action->drop_packet);
	hnae3_set_bit(ad_data, HCLGE_FD_AD_DIRECT_QID_B,
		      action->forward_to_direct_queue);
	hnae3_set_field(ad_data, HCLGE_FD_AD_QID_M, HCLGE_FD_AD_QID_S,
			action->queue_id);
	hnae3_set_bit(ad_data, HCLGE_FD_AD_USE_COUNTER_B, action->use_counter);
	hnae3_set_field(ad_data, HCLGE_FD_AD_COUNTER_NUM_M,
			HCLGE_FD_AD_COUNTER_NUM_S, action->counter_id);
	hnae3_set_bit(ad_data, HCLGE_FD_AD_NXT_STEP_B, action->use_next_stage);
	hnae3_set_field(ad_data, HCLGE_FD_AD_NXT_KEY_M, HCLGE_FD_AD_NXT_KEY_S,
			action->counter_id);

	req->ad_data = cpu_to_le64(ad_data);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev, "fd ad config fail, ret=%d\n", ret);

	return ret;
}

static bool hclge_fd_convert_tuple(u32 tuple_bit, u8 *key_x, u8 *key_y,
				   struct hclge_fd_rule *rule)
{
	u16 tmp_x_s, tmp_y_s;
	u32 tmp_x_l, tmp_y_l;
	int i;

	if (rule->unused_tuple & tuple_bit)
		return true;

	switch (tuple_bit) {
	case 0:
		return false;
	case BIT(INNER_DST_MAC):
		for (i = 0; i < 6; i++) {
			calc_x(key_x[5 - i], rule->tuples.dst_mac[i],
			       rule->tuples_mask.dst_mac[i]);
			calc_y(key_y[5 - i], rule->tuples.dst_mac[i],
			       rule->tuples_mask.dst_mac[i]);
		}

		return true;
	case BIT(INNER_SRC_MAC):
		for (i = 0; i < 6; i++) {
			calc_x(key_x[5 - i], rule->tuples.src_mac[i],
			       rule->tuples.src_mac[i]);
			calc_y(key_y[5 - i], rule->tuples.src_mac[i],
			       rule->tuples.src_mac[i]);
		}

		return true;
	case BIT(INNER_VLAN_TAG_FST):
		calc_x(tmp_x_s, rule->tuples.vlan_tag1,
		       rule->tuples_mask.vlan_tag1);
		calc_y(tmp_y_s, rule->tuples.vlan_tag1,
		       rule->tuples_mask.vlan_tag1);
		*(__le16 *)key_x = cpu_to_le16(tmp_x_s);
		*(__le16 *)key_y = cpu_to_le16(tmp_y_s);

		return true;
	case BIT(INNER_ETH_TYPE):
		calc_x(tmp_x_s, rule->tuples.ether_proto,
		       rule->tuples_mask.ether_proto);
		calc_y(tmp_y_s, rule->tuples.ether_proto,
		       rule->tuples_mask.ether_proto);
		*(__le16 *)key_x = cpu_to_le16(tmp_x_s);
		*(__le16 *)key_y = cpu_to_le16(tmp_y_s);

		return true;
	case BIT(INNER_IP_TOS):
		calc_x(*key_x, rule->tuples.ip_tos, rule->tuples_mask.ip_tos);
		calc_y(*key_y, rule->tuples.ip_tos, rule->tuples_mask.ip_tos);

		return true;
	case BIT(INNER_IP_PROTO):
		calc_x(*key_x, rule->tuples.ip_proto,
		       rule->tuples_mask.ip_proto);
		calc_y(*key_y, rule->tuples.ip_proto,
		       rule->tuples_mask.ip_proto);

		return true;
	case BIT(INNER_SRC_IP):
		calc_x(tmp_x_l, rule->tuples.src_ip[3],
		       rule->tuples_mask.src_ip[3]);
		calc_y(tmp_y_l, rule->tuples.src_ip[3],
		       rule->tuples_mask.src_ip[3]);
		*(__le32 *)key_x = cpu_to_le32(tmp_x_l);
		*(__le32 *)key_y = cpu_to_le32(tmp_y_l);

		return true;
	case BIT(INNER_DST_IP):
		calc_x(tmp_x_l, rule->tuples.dst_ip[3],
		       rule->tuples_mask.dst_ip[3]);
		calc_y(tmp_y_l, rule->tuples.dst_ip[3],
		       rule->tuples_mask.dst_ip[3]);
		*(__le32 *)key_x = cpu_to_le32(tmp_x_l);
		*(__le32 *)key_y = cpu_to_le32(tmp_y_l);

		return true;
	case BIT(INNER_SRC_PORT):
		calc_x(tmp_x_s, rule->tuples.src_port,
		       rule->tuples_mask.src_port);
		calc_y(tmp_y_s, rule->tuples.src_port,
		       rule->tuples_mask.src_port);
		*(__le16 *)key_x = cpu_to_le16(tmp_x_s);
		*(__le16 *)key_y = cpu_to_le16(tmp_y_s);

		return true;
	case BIT(INNER_DST_PORT):
		calc_x(tmp_x_s, rule->tuples.dst_port,
		       rule->tuples_mask.dst_port);
		calc_y(tmp_y_s, rule->tuples.dst_port,
		       rule->tuples_mask.dst_port);
		*(__le16 *)key_x = cpu_to_le16(tmp_x_s);
		*(__le16 *)key_y = cpu_to_le16(tmp_y_s);

		return true;
	default:
		return false;
	}
}

static u32 hclge_get_port_number(enum HLCGE_PORT_TYPE port_type, u8 pf_id,
				 u8 vf_id, u8 network_port_id)
{
	u32 port_number = 0;

	if (port_type == HOST_PORT) {
		hnae3_set_field(port_number, HCLGE_PF_ID_M, HCLGE_PF_ID_S,
				pf_id);
		hnae3_set_field(port_number, HCLGE_VF_ID_M, HCLGE_VF_ID_S,
				vf_id);
		hnae3_set_bit(port_number, HCLGE_PORT_TYPE_B, HOST_PORT);
	} else {
		hnae3_set_field(port_number, HCLGE_NETWORK_PORT_ID_M,
				HCLGE_NETWORK_PORT_ID_S, network_port_id);
		hnae3_set_bit(port_number, HCLGE_PORT_TYPE_B, NETWORK_PORT);
	}

	return port_number;
}

static void hclge_fd_convert_meta_data(struct hclge_fd_key_cfg *key_cfg,
				       __le32 *key_x, __le32 *key_y,
				       struct hclge_fd_rule *rule)
{
	u32 tuple_bit, meta_data = 0, tmp_x, tmp_y, port_number;
	u8 cur_pos = 0, tuple_size, shift_bits;
	int i;

	for (i = 0; i < MAX_META_DATA; i++) {
		tuple_size = meta_data_key_info[i].key_length;
		tuple_bit = key_cfg->meta_data_active & BIT(i);

		switch (tuple_bit) {
		case BIT(ROCE_TYPE):
			hnae3_set_bit(meta_data, cur_pos, NIC_PACKET);
			cur_pos += tuple_size;
			break;
		case BIT(DST_VPORT):
			port_number = hclge_get_port_number(HOST_PORT, 0,
							    rule->vf_id, 0);
			hnae3_set_field(meta_data,
					GENMASK(cur_pos + tuple_size, cur_pos),
					cur_pos, port_number);
			cur_pos += tuple_size;
			break;
		default:
			break;
		}
	}

	calc_x(tmp_x, meta_data, 0xFFFFFFFF);
	calc_y(tmp_y, meta_data, 0xFFFFFFFF);
	shift_bits = sizeof(meta_data) * 8 - cur_pos;

	*key_x = cpu_to_le32(tmp_x << shift_bits);
	*key_y = cpu_to_le32(tmp_y << shift_bits);
}

/* A complete key is combined with meta data key and tuple key.
 * Meta data key is stored at the MSB region, and tuple key is stored at
 * the LSB region, unused bits will be filled 0.
 */
static int hclge_config_key(struct hclge_dev *hdev, u8 stage,
			    struct hclge_fd_rule *rule)
{
	struct hclge_fd_key_cfg *key_cfg = &hdev->fd_cfg.key_cfg[stage];
	u8 key_x[MAX_KEY_BYTES], key_y[MAX_KEY_BYTES];
	u8 *cur_key_x, *cur_key_y;
	int i, ret, tuple_size;
	u8 meta_data_region;

	memset(key_x, 0, sizeof(key_x));
	memset(key_y, 0, sizeof(key_y));
	cur_key_x = key_x;
	cur_key_y = key_y;

	for (i = 0 ; i < MAX_TUPLE; i++) {
		bool tuple_valid;
		u32 check_tuple;

		tuple_size = tuple_key_info[i].key_length / 8;
		check_tuple = key_cfg->tuple_active & BIT(i);

		tuple_valid = hclge_fd_convert_tuple(check_tuple, cur_key_x,
						     cur_key_y, rule);
		if (tuple_valid) {
			cur_key_x += tuple_size;
			cur_key_y += tuple_size;
		}
	}

	meta_data_region = hdev->fd_cfg.max_key_length / 8 -
			MAX_META_DATA_LENGTH / 8;

	hclge_fd_convert_meta_data(key_cfg,
				   (__le32 *)(key_x + meta_data_region),
				   (__le32 *)(key_y + meta_data_region),
				   rule);

	ret = hclge_fd_tcam_config(hdev, stage, false, rule->location, key_y,
				   true);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"fd key_y config fail, loc=%d, ret=%d\n",
			rule->queue_id, ret);
		return ret;
	}

	ret = hclge_fd_tcam_config(hdev, stage, true, rule->location, key_x,
				   true);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"fd key_x config fail, loc=%d, ret=%d\n",
			rule->queue_id, ret);
	return ret;
}

static int hclge_config_action(struct hclge_dev *hdev, u8 stage,
			       struct hclge_fd_rule *rule)
{
	struct hclge_fd_ad_data ad_data;

	ad_data.ad_id = rule->location;

	if (rule->action == HCLGE_FD_ACTION_DROP_PACKET) {
		ad_data.drop_packet = true;
		ad_data.forward_to_direct_queue = false;
		ad_data.queue_id = 0;
	} else {
		ad_data.drop_packet = false;
		ad_data.forward_to_direct_queue = true;
		ad_data.queue_id = rule->queue_id;
	}

	ad_data.use_counter = false;
	ad_data.counter_id = 0;

	ad_data.use_next_stage = false;
	ad_data.next_input_key = 0;

	ad_data.write_rule_id_to_bd = true;
	ad_data.rule_id = rule->location;

	return hclge_fd_ad_config(hdev, stage, ad_data.ad_id, &ad_data);
}

static int hclge_fd_check_spec(struct hclge_dev *hdev,
			       struct ethtool_rx_flow_spec *fs, u32 *unused)
{
	struct ethtool_tcpip4_spec *tcp_ip4_spec;
	struct ethtool_usrip4_spec *usr_ip4_spec;
	struct ethtool_tcpip6_spec *tcp_ip6_spec;
	struct ethtool_usrip6_spec *usr_ip6_spec;
	struct ethhdr *ether_spec;

	if (fs->location >= hdev->fd_cfg.rule_num[HCLGE_FD_STAGE_1])
		return -EINVAL;

	if (!(fs->flow_type & hdev->fd_cfg.proto_support))
		return -EOPNOTSUPP;

	if ((fs->flow_type & FLOW_EXT) &&
	    (fs->h_ext.data[0] != 0 || fs->h_ext.data[1] != 0)) {
		dev_err(&hdev->pdev->dev, "user-def bytes are not supported\n");
		return -EOPNOTSUPP;
	}

	switch (fs->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT)) {
	case SCTP_V4_FLOW:
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
		tcp_ip4_spec = &fs->h_u.tcp_ip4_spec;
		*unused |= BIT(INNER_SRC_MAC) | BIT(INNER_DST_MAC);

		if (!tcp_ip4_spec->ip4src)
			*unused |= BIT(INNER_SRC_IP);

		if (!tcp_ip4_spec->ip4dst)
			*unused |= BIT(INNER_DST_IP);

		if (!tcp_ip4_spec->psrc)
			*unused |= BIT(INNER_SRC_PORT);

		if (!tcp_ip4_spec->pdst)
			*unused |= BIT(INNER_DST_PORT);

		if (!tcp_ip4_spec->tos)
			*unused |= BIT(INNER_IP_TOS);

		break;
	case IP_USER_FLOW:
		usr_ip4_spec = &fs->h_u.usr_ip4_spec;
		*unused |= BIT(INNER_SRC_MAC) | BIT(INNER_DST_MAC) |
			BIT(INNER_SRC_PORT) | BIT(INNER_DST_PORT);

		if (!usr_ip4_spec->ip4src)
			*unused |= BIT(INNER_SRC_IP);

		if (!usr_ip4_spec->ip4dst)
			*unused |= BIT(INNER_DST_IP);

		if (!usr_ip4_spec->tos)
			*unused |= BIT(INNER_IP_TOS);

		if (!usr_ip4_spec->proto)
			*unused |= BIT(INNER_IP_PROTO);

		if (usr_ip4_spec->l4_4_bytes)
			return -EOPNOTSUPP;

		if (usr_ip4_spec->ip_ver != ETH_RX_NFC_IP4)
			return -EOPNOTSUPP;

		break;
	case SCTP_V6_FLOW:
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
		tcp_ip6_spec = &fs->h_u.tcp_ip6_spec;
		*unused |= BIT(INNER_SRC_MAC) | BIT(INNER_DST_MAC) |
			BIT(INNER_IP_TOS);

		if (!tcp_ip6_spec->ip6src[0] && !tcp_ip6_spec->ip6src[1] &&
		    !tcp_ip6_spec->ip6src[2] && !tcp_ip6_spec->ip6src[3])
			*unused |= BIT(INNER_SRC_IP);

		if (!tcp_ip6_spec->ip6dst[0] && !tcp_ip6_spec->ip6dst[1] &&
		    !tcp_ip6_spec->ip6dst[2] && !tcp_ip6_spec->ip6dst[3])
			*unused |= BIT(INNER_DST_IP);

		if (!tcp_ip6_spec->psrc)
			*unused |= BIT(INNER_SRC_PORT);

		if (!tcp_ip6_spec->pdst)
			*unused |= BIT(INNER_DST_PORT);

		if (tcp_ip6_spec->tclass)
			return -EOPNOTSUPP;

		break;
	case IPV6_USER_FLOW:
		usr_ip6_spec = &fs->h_u.usr_ip6_spec;
		*unused |= BIT(INNER_SRC_MAC) | BIT(INNER_DST_MAC) |
			BIT(INNER_IP_TOS) | BIT(INNER_SRC_PORT) |
			BIT(INNER_DST_PORT);

		if (!usr_ip6_spec->ip6src[0] && !usr_ip6_spec->ip6src[1] &&
		    !usr_ip6_spec->ip6src[2] && !usr_ip6_spec->ip6src[3])
			*unused |= BIT(INNER_SRC_IP);

		if (!usr_ip6_spec->ip6dst[0] && !usr_ip6_spec->ip6dst[1] &&
		    !usr_ip6_spec->ip6dst[2] && !usr_ip6_spec->ip6dst[3])
			*unused |= BIT(INNER_DST_IP);

		if (!usr_ip6_spec->l4_proto)
			*unused |= BIT(INNER_IP_PROTO);

		if (usr_ip6_spec->tclass)
			return -EOPNOTSUPP;

		if (usr_ip6_spec->l4_4_bytes)
			return -EOPNOTSUPP;

		break;
	case ETHER_FLOW:
		ether_spec = &fs->h_u.ether_spec;
		*unused |= BIT(INNER_SRC_IP) | BIT(INNER_DST_IP) |
			BIT(INNER_SRC_PORT) | BIT(INNER_DST_PORT) |
			BIT(INNER_IP_TOS) | BIT(INNER_IP_PROTO);

		if (is_zero_ether_addr(ether_spec->h_source))
			*unused |= BIT(INNER_SRC_MAC);

		if (is_zero_ether_addr(ether_spec->h_dest))
			*unused |= BIT(INNER_DST_MAC);

		if (!ether_spec->h_proto)
			*unused |= BIT(INNER_ETH_TYPE);

		break;
	default:
		return -EOPNOTSUPP;
	}

	if ((fs->flow_type & FLOW_EXT)) {
		if (fs->h_ext.vlan_etype)
			return -EOPNOTSUPP;
		if (!fs->h_ext.vlan_tci)
			*unused |= BIT(INNER_VLAN_TAG_FST);

		if (fs->m_ext.vlan_tci) {
			if (be16_to_cpu(fs->h_ext.vlan_tci) >= VLAN_N_VID)
				return -EINVAL;
		}
	} else {
		*unused |= BIT(INNER_VLAN_TAG_FST);
	}

	if (fs->flow_type & FLOW_MAC_EXT) {
		if (!(hdev->fd_cfg.proto_support & ETHER_FLOW))
			return -EOPNOTSUPP;

		if (is_zero_ether_addr(fs->h_ext.h_dest))
			*unused |= BIT(INNER_DST_MAC);
		else
			*unused &= ~(BIT(INNER_DST_MAC));
	}

	return 0;
}

static bool hclge_fd_rule_exist(struct hclge_dev *hdev, u16 location)
{
	struct hclge_fd_rule *rule = NULL;
	struct hlist_node *node2;

	hlist_for_each_entry_safe(rule, node2, &hdev->fd_rule_list, rule_node) {
		if (rule->location >= location)
			break;
	}

	return  rule && rule->location == location;
}

static int hclge_fd_update_rule_list(struct hclge_dev *hdev,
				     struct hclge_fd_rule *new_rule,
				     u16 location,
				     bool is_add)
{
	struct hclge_fd_rule *rule = NULL, *parent = NULL;
	struct hlist_node *node2;

	if (is_add && !new_rule)
		return -EINVAL;

	hlist_for_each_entry_safe(rule, node2,
				  &hdev->fd_rule_list, rule_node) {
		if (rule->location >= location)
			break;
		parent = rule;
	}

	if (rule && rule->location == location) {
		hlist_del(&rule->rule_node);
		kfree(rule);
		hdev->hclge_fd_rule_num--;

		if (!is_add)
			return 0;

	} else if (!is_add) {
		dev_err(&hdev->pdev->dev,
			"delete fail, rule %d is inexistent\n",
			location);
		return -EINVAL;
	}

	INIT_HLIST_NODE(&new_rule->rule_node);

	if (parent)
		hlist_add_behind(&new_rule->rule_node, &parent->rule_node);
	else
		hlist_add_head(&new_rule->rule_node, &hdev->fd_rule_list);

	hdev->hclge_fd_rule_num++;

	return 0;
}

static int hclge_fd_get_tuple(struct hclge_dev *hdev,
			      struct ethtool_rx_flow_spec *fs,
			      struct hclge_fd_rule *rule)
{
	u32 flow_type = fs->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT);

	switch (flow_type) {
	case SCTP_V4_FLOW:
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
		rule->tuples.src_ip[3] =
				be32_to_cpu(fs->h_u.tcp_ip4_spec.ip4src);
		rule->tuples_mask.src_ip[3] =
				be32_to_cpu(fs->m_u.tcp_ip4_spec.ip4src);

		rule->tuples.dst_ip[3] =
				be32_to_cpu(fs->h_u.tcp_ip4_spec.ip4dst);
		rule->tuples_mask.dst_ip[3] =
				be32_to_cpu(fs->m_u.tcp_ip4_spec.ip4dst);

		rule->tuples.src_port = be16_to_cpu(fs->h_u.tcp_ip4_spec.psrc);
		rule->tuples_mask.src_port =
				be16_to_cpu(fs->m_u.tcp_ip4_spec.psrc);

		rule->tuples.dst_port = be16_to_cpu(fs->h_u.tcp_ip4_spec.pdst);
		rule->tuples_mask.dst_port =
				be16_to_cpu(fs->m_u.tcp_ip4_spec.pdst);

		rule->tuples.ip_tos = fs->h_u.tcp_ip4_spec.tos;
		rule->tuples_mask.ip_tos = fs->m_u.tcp_ip4_spec.tos;

		rule->tuples.ether_proto = ETH_P_IP;
		rule->tuples_mask.ether_proto = 0xFFFF;

		break;
	case IP_USER_FLOW:
		rule->tuples.src_ip[3] =
				be32_to_cpu(fs->h_u.usr_ip4_spec.ip4src);
		rule->tuples_mask.src_ip[3] =
				be32_to_cpu(fs->m_u.usr_ip4_spec.ip4src);

		rule->tuples.dst_ip[3] =
				be32_to_cpu(fs->h_u.usr_ip4_spec.ip4dst);
		rule->tuples_mask.dst_ip[3] =
				be32_to_cpu(fs->m_u.usr_ip4_spec.ip4dst);

		rule->tuples.ip_tos = fs->h_u.usr_ip4_spec.tos;
		rule->tuples_mask.ip_tos = fs->m_u.usr_ip4_spec.tos;

		rule->tuples.ip_proto = fs->h_u.usr_ip4_spec.proto;
		rule->tuples_mask.ip_proto = fs->m_u.usr_ip4_spec.proto;

		rule->tuples.ether_proto = ETH_P_IP;
		rule->tuples_mask.ether_proto = 0xFFFF;

		break;
	case SCTP_V6_FLOW:
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
		be32_to_cpu_array(rule->tuples.src_ip,
				  fs->h_u.tcp_ip6_spec.ip6src, 4);
		be32_to_cpu_array(rule->tuples_mask.src_ip,
				  fs->m_u.tcp_ip6_spec.ip6src, 4);

		be32_to_cpu_array(rule->tuples.dst_ip,
				  fs->h_u.tcp_ip6_spec.ip6dst, 4);
		be32_to_cpu_array(rule->tuples_mask.dst_ip,
				  fs->m_u.tcp_ip6_spec.ip6dst, 4);

		rule->tuples.src_port = be16_to_cpu(fs->h_u.tcp_ip6_spec.psrc);
		rule->tuples_mask.src_port =
				be16_to_cpu(fs->m_u.tcp_ip6_spec.psrc);

		rule->tuples.dst_port = be16_to_cpu(fs->h_u.tcp_ip6_spec.pdst);
		rule->tuples_mask.dst_port =
				be16_to_cpu(fs->m_u.tcp_ip6_spec.pdst);

		rule->tuples.ether_proto = ETH_P_IPV6;
		rule->tuples_mask.ether_proto = 0xFFFF;

		break;
	case IPV6_USER_FLOW:
		be32_to_cpu_array(rule->tuples.src_ip,
				  fs->h_u.usr_ip6_spec.ip6src, 4);
		be32_to_cpu_array(rule->tuples_mask.src_ip,
				  fs->m_u.usr_ip6_spec.ip6src, 4);

		be32_to_cpu_array(rule->tuples.dst_ip,
				  fs->h_u.usr_ip6_spec.ip6dst, 4);
		be32_to_cpu_array(rule->tuples_mask.dst_ip,
				  fs->m_u.usr_ip6_spec.ip6dst, 4);

		rule->tuples.ip_proto = fs->h_u.usr_ip6_spec.l4_proto;
		rule->tuples_mask.ip_proto = fs->m_u.usr_ip6_spec.l4_proto;

		rule->tuples.ether_proto = ETH_P_IPV6;
		rule->tuples_mask.ether_proto = 0xFFFF;

		break;
	case ETHER_FLOW:
		ether_addr_copy(rule->tuples.src_mac,
				fs->h_u.ether_spec.h_source);
		ether_addr_copy(rule->tuples_mask.src_mac,
				fs->m_u.ether_spec.h_source);

		ether_addr_copy(rule->tuples.dst_mac,
				fs->h_u.ether_spec.h_dest);
		ether_addr_copy(rule->tuples_mask.dst_mac,
				fs->m_u.ether_spec.h_dest);

		rule->tuples.ether_proto =
				be16_to_cpu(fs->h_u.ether_spec.h_proto);
		rule->tuples_mask.ether_proto =
				be16_to_cpu(fs->m_u.ether_spec.h_proto);

		break;
	default:
		return -EOPNOTSUPP;
	}

	switch (flow_type) {
	case SCTP_V4_FLOW:
	case SCTP_V6_FLOW:
		rule->tuples.ip_proto = IPPROTO_SCTP;
		rule->tuples_mask.ip_proto = 0xFF;
		break;
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
		rule->tuples.ip_proto = IPPROTO_TCP;
		rule->tuples_mask.ip_proto = 0xFF;
		break;
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
		rule->tuples.ip_proto = IPPROTO_UDP;
		rule->tuples_mask.ip_proto = 0xFF;
		break;
	default:
		break;
	}

	if ((fs->flow_type & FLOW_EXT)) {
		rule->tuples.vlan_tag1 = be16_to_cpu(fs->h_ext.vlan_tci);
		rule->tuples_mask.vlan_tag1 = be16_to_cpu(fs->m_ext.vlan_tci);
	}

	if (fs->flow_type & FLOW_MAC_EXT) {
		ether_addr_copy(rule->tuples.dst_mac, fs->h_ext.h_dest);
		ether_addr_copy(rule->tuples_mask.dst_mac, fs->m_ext.h_dest);
	}

	return 0;
}

static int hclge_add_fd_entry(struct hnae3_handle *handle,
			      struct ethtool_rxnfc *cmd)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	u16 dst_vport_id = 0, q_index = 0;
	struct ethtool_rx_flow_spec *fs;
	struct hclge_fd_rule *rule;
	u32 unused = 0;
	u8 action;
	int ret;

	if (!hnae3_dev_fd_supported(hdev))
		return -EOPNOTSUPP;

	if (!hdev->fd_en) {
		dev_warn(&hdev->pdev->dev,
			 "Please enable flow director first\n");
		return -EOPNOTSUPP;
	}

	fs = (struct ethtool_rx_flow_spec *)&cmd->fs;

	ret = hclge_fd_check_spec(hdev, fs, &unused);
	if (ret) {
		dev_err(&hdev->pdev->dev, "Check fd spec failed\n");
		return ret;
	}

	if (fs->ring_cookie == RX_CLS_FLOW_DISC) {
		action = HCLGE_FD_ACTION_DROP_PACKET;
	} else {
		u32 ring = ethtool_get_flow_spec_ring(fs->ring_cookie);
		u8 vf = ethtool_get_flow_spec_ring_vf(fs->ring_cookie);
		u16 tqps;

		if (vf > hdev->num_req_vfs) {
			dev_err(&hdev->pdev->dev,
				"Error: vf id (%d) > max vf num (%d)\n",
				vf, hdev->num_req_vfs);
			return -EINVAL;
		}

		dst_vport_id = vf ? hdev->vport[vf].vport_id : vport->vport_id;
		tqps = vf ? hdev->vport[vf].alloc_tqps : vport->alloc_tqps;

		if (ring >= tqps) {
			dev_err(&hdev->pdev->dev,
				"Error: queue id (%d) > max tqp num (%d)\n",
				ring, tqps - 1);
			return -EINVAL;
		}

		action = HCLGE_FD_ACTION_ACCEPT_PACKET;
		q_index = ring;
	}

	rule = kzalloc(sizeof(*rule), GFP_KERNEL);
	if (!rule)
		return -ENOMEM;

	ret = hclge_fd_get_tuple(hdev, fs, rule);
	if (ret)
		goto free_rule;

	rule->flow_type = fs->flow_type;

	rule->location = fs->location;
	rule->unused_tuple = unused;
	rule->vf_id = dst_vport_id;
	rule->queue_id = q_index;
	rule->action = action;

	ret = hclge_config_action(hdev, HCLGE_FD_STAGE_1, rule);
	if (ret)
		goto free_rule;

	ret = hclge_config_key(hdev, HCLGE_FD_STAGE_1, rule);
	if (ret)
		goto free_rule;

	ret = hclge_fd_update_rule_list(hdev, rule, fs->location, true);
	if (ret)
		goto free_rule;

	return ret;

free_rule:
	kfree(rule);
	return ret;
}

static int hclge_del_fd_entry(struct hnae3_handle *handle,
			      struct ethtool_rxnfc *cmd)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct ethtool_rx_flow_spec *fs;
	int ret;

	if (!hnae3_dev_fd_supported(hdev))
		return -EOPNOTSUPP;

	fs = (struct ethtool_rx_flow_spec *)&cmd->fs;

	if (fs->location >= hdev->fd_cfg.rule_num[HCLGE_FD_STAGE_1])
		return -EINVAL;

	if (!hclge_fd_rule_exist(hdev, fs->location)) {
		dev_err(&hdev->pdev->dev,
			"Delete fail, rule %d is inexistent\n",
			fs->location);
		return -ENOENT;
	}

	ret = hclge_fd_tcam_config(hdev, HCLGE_FD_STAGE_1, true,
				   fs->location, NULL, false);
	if (ret)
		return ret;

	return hclge_fd_update_rule_list(hdev, NULL, fs->location,
					 false);
}

static void hclge_del_all_fd_entries(struct hnae3_handle *handle,
				     bool clear_list)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hclge_fd_rule *rule;
	struct hlist_node *node;

	if (!hnae3_dev_fd_supported(hdev))
		return;

	if (clear_list) {
		hlist_for_each_entry_safe(rule, node, &hdev->fd_rule_list,
					  rule_node) {
			hclge_fd_tcam_config(hdev, HCLGE_FD_STAGE_1, true,
					     rule->location, NULL, false);
			hlist_del(&rule->rule_node);
			kfree(rule);
			hdev->hclge_fd_rule_num--;
		}
	} else {
		hlist_for_each_entry_safe(rule, node, &hdev->fd_rule_list,
					  rule_node)
			hclge_fd_tcam_config(hdev, HCLGE_FD_STAGE_1, true,
					     rule->location, NULL, false);
	}
}

static int hclge_restore_fd_entries(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hclge_fd_rule *rule;
	struct hlist_node *node;
	int ret;

	/* Return ok here, because reset error handling will check this
	 * return value. If error is returned here, the reset process will
	 * fail.
	 */
	if (!hnae3_dev_fd_supported(hdev))
		return 0;

	/* if fd is disabled, should not restore it when reset */
	if (!hdev->fd_en)
		return 0;

	hlist_for_each_entry_safe(rule, node, &hdev->fd_rule_list, rule_node) {
		ret = hclge_config_action(hdev, HCLGE_FD_STAGE_1, rule);
		if (!ret)
			ret = hclge_config_key(hdev, HCLGE_FD_STAGE_1, rule);

		if (ret) {
			dev_warn(&hdev->pdev->dev,
				 "Restore rule %d failed, remove it\n",
				 rule->location);
			hlist_del(&rule->rule_node);
			kfree(rule);
			hdev->hclge_fd_rule_num--;
		}
	}
	return 0;
}

static int hclge_get_fd_rule_cnt(struct hnae3_handle *handle,
				 struct ethtool_rxnfc *cmd)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (!hnae3_dev_fd_supported(hdev))
		return -EOPNOTSUPP;

	cmd->rule_cnt = hdev->hclge_fd_rule_num;
	cmd->data = hdev->fd_cfg.rule_num[HCLGE_FD_STAGE_1];

	return 0;
}

static int hclge_get_fd_rule_info(struct hnae3_handle *handle,
				  struct ethtool_rxnfc *cmd)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_fd_rule *rule = NULL;
	struct hclge_dev *hdev = vport->back;
	struct ethtool_rx_flow_spec *fs;
	struct hlist_node *node2;

	if (!hnae3_dev_fd_supported(hdev))
		return -EOPNOTSUPP;

	fs = (struct ethtool_rx_flow_spec *)&cmd->fs;

	hlist_for_each_entry_safe(rule, node2, &hdev->fd_rule_list, rule_node) {
		if (rule->location >= fs->location)
			break;
	}

	if (!rule || fs->location != rule->location)
		return -ENOENT;

	fs->flow_type = rule->flow_type;
	switch (fs->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT)) {
	case SCTP_V4_FLOW:
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
		fs->h_u.tcp_ip4_spec.ip4src =
				cpu_to_be32(rule->tuples.src_ip[3]);
		fs->m_u.tcp_ip4_spec.ip4src =
				rule->unused_tuple & BIT(INNER_SRC_IP) ?
				0 : cpu_to_be32(rule->tuples_mask.src_ip[3]);

		fs->h_u.tcp_ip4_spec.ip4dst =
				cpu_to_be32(rule->tuples.dst_ip[3]);
		fs->m_u.tcp_ip4_spec.ip4dst =
				rule->unused_tuple & BIT(INNER_DST_IP) ?
				0 : cpu_to_be32(rule->tuples_mask.dst_ip[3]);

		fs->h_u.tcp_ip4_spec.psrc = cpu_to_be16(rule->tuples.src_port);
		fs->m_u.tcp_ip4_spec.psrc =
				rule->unused_tuple & BIT(INNER_SRC_PORT) ?
				0 : cpu_to_be16(rule->tuples_mask.src_port);

		fs->h_u.tcp_ip4_spec.pdst = cpu_to_be16(rule->tuples.dst_port);
		fs->m_u.tcp_ip4_spec.pdst =
				rule->unused_tuple & BIT(INNER_DST_PORT) ?
				0 : cpu_to_be16(rule->tuples_mask.dst_port);

		fs->h_u.tcp_ip4_spec.tos = rule->tuples.ip_tos;
		fs->m_u.tcp_ip4_spec.tos =
				rule->unused_tuple & BIT(INNER_IP_TOS) ?
				0 : rule->tuples_mask.ip_tos;

		break;
	case IP_USER_FLOW:
		fs->h_u.usr_ip4_spec.ip4src =
				cpu_to_be32(rule->tuples.src_ip[3]);
		fs->m_u.tcp_ip4_spec.ip4src =
				rule->unused_tuple & BIT(INNER_SRC_IP) ?
				0 : cpu_to_be32(rule->tuples_mask.src_ip[3]);

		fs->h_u.usr_ip4_spec.ip4dst =
				cpu_to_be32(rule->tuples.dst_ip[3]);
		fs->m_u.usr_ip4_spec.ip4dst =
				rule->unused_tuple & BIT(INNER_DST_IP) ?
				0 : cpu_to_be32(rule->tuples_mask.dst_ip[3]);

		fs->h_u.usr_ip4_spec.tos = rule->tuples.ip_tos;
		fs->m_u.usr_ip4_spec.tos =
				rule->unused_tuple & BIT(INNER_IP_TOS) ?
				0 : rule->tuples_mask.ip_tos;

		fs->h_u.usr_ip4_spec.proto = rule->tuples.ip_proto;
		fs->m_u.usr_ip4_spec.proto =
				rule->unused_tuple & BIT(INNER_IP_PROTO) ?
				0 : rule->tuples_mask.ip_proto;

		fs->h_u.usr_ip4_spec.ip_ver = ETH_RX_NFC_IP4;

		break;
	case SCTP_V6_FLOW:
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
		cpu_to_be32_array(fs->h_u.tcp_ip6_spec.ip6src,
				  rule->tuples.src_ip, 4);
		if (rule->unused_tuple & BIT(INNER_SRC_IP))
			memset(fs->m_u.tcp_ip6_spec.ip6src, 0, sizeof(int) * 4);
		else
			cpu_to_be32_array(fs->m_u.tcp_ip6_spec.ip6src,
					  rule->tuples_mask.src_ip, 4);

		cpu_to_be32_array(fs->h_u.tcp_ip6_spec.ip6dst,
				  rule->tuples.dst_ip, 4);
		if (rule->unused_tuple & BIT(INNER_DST_IP))
			memset(fs->m_u.tcp_ip6_spec.ip6dst, 0, sizeof(int) * 4);
		else
			cpu_to_be32_array(fs->m_u.tcp_ip6_spec.ip6dst,
					  rule->tuples_mask.dst_ip, 4);

		fs->h_u.tcp_ip6_spec.psrc = cpu_to_be16(rule->tuples.src_port);
		fs->m_u.tcp_ip6_spec.psrc =
				rule->unused_tuple & BIT(INNER_SRC_PORT) ?
				0 : cpu_to_be16(rule->tuples_mask.src_port);

		fs->h_u.tcp_ip6_spec.pdst = cpu_to_be16(rule->tuples.dst_port);
		fs->m_u.tcp_ip6_spec.pdst =
				rule->unused_tuple & BIT(INNER_DST_PORT) ?
				0 : cpu_to_be16(rule->tuples_mask.dst_port);

		break;
	case IPV6_USER_FLOW:
		cpu_to_be32_array(fs->h_u.usr_ip6_spec.ip6src,
				  rule->tuples.src_ip, 4);
		if (rule->unused_tuple & BIT(INNER_SRC_IP))
			memset(fs->m_u.usr_ip6_spec.ip6src, 0, sizeof(int) * 4);
		else
			cpu_to_be32_array(fs->m_u.usr_ip6_spec.ip6src,
					  rule->tuples_mask.src_ip, 4);

		cpu_to_be32_array(fs->h_u.usr_ip6_spec.ip6dst,
				  rule->tuples.dst_ip, 4);
		if (rule->unused_tuple & BIT(INNER_DST_IP))
			memset(fs->m_u.usr_ip6_spec.ip6dst, 0, sizeof(int) * 4);
		else
			cpu_to_be32_array(fs->m_u.usr_ip6_spec.ip6dst,
					  rule->tuples_mask.dst_ip, 4);

		fs->h_u.usr_ip6_spec.l4_proto = rule->tuples.ip_proto;
		fs->m_u.usr_ip6_spec.l4_proto =
				rule->unused_tuple & BIT(INNER_IP_PROTO) ?
				0 : rule->tuples_mask.ip_proto;

		break;
	case ETHER_FLOW:
		ether_addr_copy(fs->h_u.ether_spec.h_source,
				rule->tuples.src_mac);
		if (rule->unused_tuple & BIT(INNER_SRC_MAC))
			eth_zero_addr(fs->m_u.ether_spec.h_source);
		else
			ether_addr_copy(fs->m_u.ether_spec.h_source,
					rule->tuples_mask.src_mac);

		ether_addr_copy(fs->h_u.ether_spec.h_dest,
				rule->tuples.dst_mac);
		if (rule->unused_tuple & BIT(INNER_DST_MAC))
			eth_zero_addr(fs->m_u.ether_spec.h_dest);
		else
			ether_addr_copy(fs->m_u.ether_spec.h_dest,
					rule->tuples_mask.dst_mac);

		fs->h_u.ether_spec.h_proto =
				cpu_to_be16(rule->tuples.ether_proto);
		fs->m_u.ether_spec.h_proto =
				rule->unused_tuple & BIT(INNER_ETH_TYPE) ?
				0 : cpu_to_be16(rule->tuples_mask.ether_proto);

		break;
	default:
		return -EOPNOTSUPP;
	}

	if (fs->flow_type & FLOW_EXT) {
		fs->h_ext.vlan_tci = cpu_to_be16(rule->tuples.vlan_tag1);
		fs->m_ext.vlan_tci =
				rule->unused_tuple & BIT(INNER_VLAN_TAG_FST) ?
				cpu_to_be16(VLAN_VID_MASK) :
				cpu_to_be16(rule->tuples_mask.vlan_tag1);
	}

	if (fs->flow_type & FLOW_MAC_EXT) {
		ether_addr_copy(fs->h_ext.h_dest, rule->tuples.dst_mac);
		if (rule->unused_tuple & BIT(INNER_DST_MAC))
			eth_zero_addr(fs->m_u.ether_spec.h_dest);
		else
			ether_addr_copy(fs->m_u.ether_spec.h_dest,
					rule->tuples_mask.dst_mac);
	}

	if (rule->action == HCLGE_FD_ACTION_DROP_PACKET) {
		fs->ring_cookie = RX_CLS_FLOW_DISC;
	} else {
		u64 vf_id;

		fs->ring_cookie = rule->queue_id;
		vf_id = rule->vf_id;
		vf_id <<= ETHTOOL_RX_FLOW_SPEC_RING_VF_OFF;
		fs->ring_cookie |= vf_id;
	}

	return 0;
}

static int hclge_get_all_rules(struct hnae3_handle *handle,
			       struct ethtool_rxnfc *cmd, u32 *rule_locs)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hclge_fd_rule *rule;
	struct hlist_node *node2;
	int cnt = 0;

	if (!hnae3_dev_fd_supported(hdev))
		return -EOPNOTSUPP;

	cmd->data = hdev->fd_cfg.rule_num[HCLGE_FD_STAGE_1];

	hlist_for_each_entry_safe(rule, node2,
				  &hdev->fd_rule_list, rule_node) {
		if (cnt == cmd->rule_cnt)
			return -EMSGSIZE;

		rule_locs[cnt] = rule->location;
		cnt++;
	}

	cmd->rule_cnt = cnt;

	return 0;
}

static bool hclge_get_hw_reset_stat(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hclge_read_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG) ||
	       hclge_read_dev(&hdev->hw, HCLGE_FUN_RST_ING);
}

static bool hclge_ae_dev_resetting(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return test_bit(HCLGE_STATE_RST_HANDLING, &hdev->state);
}

static unsigned long hclge_ae_dev_reset_cnt(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hdev->reset_count;
}

static void hclge_enable_fd(struct hnae3_handle *handle, bool enable)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	hdev->fd_en = enable;
	if (!enable)
		hclge_del_all_fd_entries(handle, false);
	else
		hclge_restore_fd_entries(handle);
}

static void hclge_cfg_mac_mode(struct hclge_dev *hdev, bool enable)
{
	struct hclge_desc desc;
	struct hclge_config_mac_mode_cmd *req =
		(struct hclge_config_mac_mode_cmd *)desc.data;
	u32 loop_en = 0;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CONFIG_MAC_MODE, false);
	hnae3_set_bit(loop_en, HCLGE_MAC_TX_EN_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_RX_EN_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_PAD_TX_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_PAD_RX_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_1588_TX_B, 0);
	hnae3_set_bit(loop_en, HCLGE_MAC_1588_RX_B, 0);
	hnae3_set_bit(loop_en, HCLGE_MAC_APP_LP_B, 0);
	hnae3_set_bit(loop_en, HCLGE_MAC_LINE_LP_B, 0);
	hnae3_set_bit(loop_en, HCLGE_MAC_FCS_TX_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_RX_FCS_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_RX_FCS_STRIP_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_TX_OVERSIZE_TRUNCATE_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_RX_OVERSIZE_TRUNCATE_B, enable);
	hnae3_set_bit(loop_en, HCLGE_MAC_TX_UNDER_MIN_ERR_B, enable);
	req->txrx_pad_fcs_loop_en = cpu_to_le32(loop_en);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"mac enable fail, ret =%d.\n", ret);
}

static int hclge_set_app_loopback(struct hclge_dev *hdev, bool en)
{
	struct hclge_config_mac_mode_cmd *req;
	struct hclge_desc desc;
	u32 loop_en;
	int ret;

	req = (struct hclge_config_mac_mode_cmd *)&desc.data[0];
	/* 1 Read out the MAC mode config at first */
	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CONFIG_MAC_MODE, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"mac loopback get fail, ret =%d.\n", ret);
		return ret;
	}

	/* 2 Then setup the loopback flag */
	loop_en = le32_to_cpu(req->txrx_pad_fcs_loop_en);
	hnae3_set_bit(loop_en, HCLGE_MAC_APP_LP_B, en ? 1 : 0);
	hnae3_set_bit(loop_en, HCLGE_MAC_TX_EN_B, en ? 1 : 0);
	hnae3_set_bit(loop_en, HCLGE_MAC_RX_EN_B, en ? 1 : 0);

	req->txrx_pad_fcs_loop_en = cpu_to_le32(loop_en);

	/* 3 Config mac work mode with loopback flag
	 * and its original configure parameters
	 */
	hclge_cmd_reuse_desc(&desc, false);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"mac loopback set fail, ret =%d.\n", ret);
	return ret;
}

static int hclge_set_serdes_loopback(struct hclge_dev *hdev, bool en,
				     enum hnae3_loop loop_mode)
{
#define HCLGE_SERDES_RETRY_MS	10
#define HCLGE_SERDES_RETRY_NUM	100

#define HCLGE_MAC_LINK_STATUS_MS   20
#define HCLGE_MAC_LINK_STATUS_NUM  10
#define HCLGE_MAC_LINK_STATUS_DOWN 0
#define HCLGE_MAC_LINK_STATUS_UP   1

	struct hclge_serdes_lb_cmd *req;
	struct hclge_desc desc;
	int mac_link_ret = 0;
	int ret, i = 0;
	u8 loop_mode_b;

	req = (struct hclge_serdes_lb_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_SERDES_LOOPBACK, false);

	switch (loop_mode) {
	case HNAE3_LOOP_SERIAL_SERDES:
		loop_mode_b = HCLGE_CMD_SERDES_SERIAL_INNER_LOOP_B;
		break;
	case HNAE3_LOOP_PARALLEL_SERDES:
		loop_mode_b = HCLGE_CMD_SERDES_PARALLEL_INNER_LOOP_B;
		break;
	default:
		dev_err(&hdev->pdev->dev,
			"unsupported serdes loopback mode %d\n", loop_mode);
		return -ENOTSUPP;
	}

	if (en) {
		req->enable = loop_mode_b;
		req->mask = loop_mode_b;
		mac_link_ret = HCLGE_MAC_LINK_STATUS_UP;
	} else {
		req->mask = loop_mode_b;
		mac_link_ret = HCLGE_MAC_LINK_STATUS_DOWN;
	}

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"serdes loopback set fail, ret = %d\n", ret);
		return ret;
	}

	do {
		msleep(HCLGE_SERDES_RETRY_MS);
		hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_SERDES_LOOPBACK,
					   true);
		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"serdes loopback get, ret = %d\n", ret);
			return ret;
		}
	} while (++i < HCLGE_SERDES_RETRY_NUM &&
		 !(req->result & HCLGE_CMD_SERDES_DONE_B));

	if (!(req->result & HCLGE_CMD_SERDES_DONE_B)) {
		dev_err(&hdev->pdev->dev, "serdes loopback set timeout\n");
		return -EBUSY;
	} else if (!(req->result & HCLGE_CMD_SERDES_SUCCESS_B)) {
		dev_err(&hdev->pdev->dev, "serdes loopback set failed in fw\n");
		return -EIO;
	}

	hclge_cfg_mac_mode(hdev, en);

	i = 0;
	do {
		/* serdes Internal loopback, independent of the network cable.*/
		msleep(HCLGE_MAC_LINK_STATUS_MS);
		ret = hclge_get_mac_link_status(hdev);
		if (ret == mac_link_ret)
			return 0;
	} while (++i < HCLGE_MAC_LINK_STATUS_NUM);

	dev_err(&hdev->pdev->dev, "config mac mode timeout\n");

	return -EBUSY;
}

static int hclge_tqp_enable(struct hclge_dev *hdev, int tqp_id,
			    int stream_id, bool enable)
{
	struct hclge_desc desc;
	struct hclge_cfg_com_tqp_queue_cmd *req =
		(struct hclge_cfg_com_tqp_queue_cmd *)desc.data;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CFG_COM_TQP_QUEUE, false);
	req->tqp_id = cpu_to_le16(tqp_id & HCLGE_RING_ID_MASK);
	req->stream_id = cpu_to_le16(stream_id);
	req->enable |= enable << HCLGE_TQP_ENABLE_B;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Tqp enable fail, status =%d.\n", ret);
	return ret;
}

static int hclge_set_loopback(struct hnae3_handle *handle,
			      enum hnae3_loop loop_mode, bool en)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hnae3_knic_private_info *kinfo;
	struct hclge_dev *hdev = vport->back;
	int i, ret;

	switch (loop_mode) {
	case HNAE3_LOOP_APP:
		ret = hclge_set_app_loopback(hdev, en);
		break;
	case HNAE3_LOOP_SERIAL_SERDES:
	case HNAE3_LOOP_PARALLEL_SERDES:
		ret = hclge_set_serdes_loopback(hdev, en, loop_mode);
		break;
	default:
		ret = -ENOTSUPP;
		dev_err(&hdev->pdev->dev,
			"loop_mode %d is not supported\n", loop_mode);
		break;
	}

	if (ret)
		return ret;

	kinfo = &vport->nic.kinfo;
	for (i = 0; i < kinfo->num_tqps; i++) {
		ret = hclge_tqp_enable(hdev, i, 0, en);
		if (ret)
			return ret;
	}

	return 0;
}

static void hclge_reset_tqp_stats(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hnae3_knic_private_info *kinfo;
	struct hnae3_queue *queue;
	struct hclge_tqp *tqp;
	int i;

	kinfo = &vport->nic.kinfo;
	for (i = 0; i < kinfo->num_tqps; i++) {
		queue = handle->kinfo.tqp[i];
		tqp = container_of(queue, struct hclge_tqp, q);
		memset(&tqp->tqp_stats, 0, sizeof(tqp->tqp_stats));
	}
}

static void hclge_set_timer_task(struct hnae3_handle *handle, bool enable)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (enable) {
		mod_timer(&hdev->service_timer, jiffies + HZ);
	} else {
		del_timer_sync(&hdev->service_timer);
		cancel_work_sync(&hdev->service_task);
		clear_bit(HCLGE_STATE_SERVICE_SCHED, &hdev->state);
	}
}

static int hclge_ae_start(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	/* mac enable */
	hclge_cfg_mac_mode(hdev, true);
	clear_bit(HCLGE_STATE_DOWN, &hdev->state);
	hdev->hw.mac.link = 0;

	/* reset tqp stats */
	hclge_reset_tqp_stats(handle);

	hclge_mac_start_phy(hdev);

	return 0;
}

static void hclge_ae_stop(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int i;

	set_bit(HCLGE_STATE_DOWN, &hdev->state);

	/* If it is not PF reset, the firmware will disable the MAC,
	 * so it only need to stop phy here.
	 */
	if (test_bit(HCLGE_STATE_RST_HANDLING, &hdev->state) &&
	    hdev->reset_type != HNAE3_FUNC_RESET) {
		hclge_mac_stop_phy(hdev);
		return;
	}

	for (i = 0; i < handle->kinfo.num_tqps; i++)
		hclge_reset_tqp(handle, i);

	/* Mac disable */
	hclge_cfg_mac_mode(hdev, false);

	hclge_mac_stop_phy(hdev);

	/* reset tqp stats */
	hclge_reset_tqp_stats(handle);
	hclge_update_link_status(hdev);
}

int hclge_vport_start(struct hclge_vport *vport)
{
	set_bit(HCLGE_VPORT_STATE_ALIVE, &vport->state);
	vport->last_active_jiffies = jiffies;
	return 0;
}

void hclge_vport_stop(struct hclge_vport *vport)
{
	clear_bit(HCLGE_VPORT_STATE_ALIVE, &vport->state);
}

static int hclge_client_start(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	return hclge_vport_start(vport);
}

static void hclge_client_stop(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	hclge_vport_stop(vport);
}

static int hclge_get_mac_vlan_cmd_status(struct hclge_vport *vport,
					 u16 cmdq_resp, u8  resp_code,
					 enum hclge_mac_vlan_tbl_opcode op)
{
	struct hclge_dev *hdev = vport->back;
	int return_status = -EIO;

	if (cmdq_resp) {
		dev_err(&hdev->pdev->dev,
			"cmdq execute failed for get_mac_vlan_cmd_status,status=%d.\n",
			cmdq_resp);
		return -EIO;
	}

	if (op == HCLGE_MAC_VLAN_ADD) {
		if ((!resp_code) || (resp_code == 1)) {
			return_status = 0;
		} else if (resp_code == 2) {
			return_status = -ENOSPC;
			dev_err(&hdev->pdev->dev,
				"add mac addr failed for uc_overflow.\n");
		} else if (resp_code == 3) {
			return_status = -ENOSPC;
			dev_err(&hdev->pdev->dev,
				"add mac addr failed for mc_overflow.\n");
		} else {
			dev_err(&hdev->pdev->dev,
				"add mac addr failed for undefined, code=%d.\n",
				resp_code);
		}
	} else if (op == HCLGE_MAC_VLAN_REMOVE) {
		if (!resp_code) {
			return_status = 0;
		} else if (resp_code == 1) {
			return_status = -ENOENT;
			dev_dbg(&hdev->pdev->dev,
				"remove mac addr failed for miss.\n");
		} else {
			dev_err(&hdev->pdev->dev,
				"remove mac addr failed for undefined, code=%d.\n",
				resp_code);
		}
	} else if (op == HCLGE_MAC_VLAN_LKUP) {
		if (!resp_code) {
			return_status = 0;
		} else if (resp_code == 1) {
			return_status = -ENOENT;
			dev_dbg(&hdev->pdev->dev,
				"lookup mac addr failed for miss.\n");
		} else {
			dev_err(&hdev->pdev->dev,
				"lookup mac addr failed for undefined, code=%d.\n",
				resp_code);
		}
	} else {
		return_status = -EINVAL;
		dev_err(&hdev->pdev->dev,
			"unknown opcode for get_mac_vlan_cmd_status,opcode=%d.\n",
			op);
	}

	return return_status;
}

static int hclge_update_desc_vfid(struct hclge_desc *desc, int vfid, bool clr)
{
	int word_num;
	int bit_num;

	if (vfid > 255 || vfid < 0)
		return -EIO;

	if (vfid >= 0 && vfid <= 191) {
		word_num = vfid / 32;
		bit_num  = vfid % 32;
		if (clr)
			desc[1].data[word_num] &= cpu_to_le32(~(1 << bit_num));
		else
			desc[1].data[word_num] |= cpu_to_le32(1 << bit_num);
	} else {
		word_num = (vfid - 192) / 32;
		bit_num  = vfid % 32;
		if (clr)
			desc[2].data[word_num] &= cpu_to_le32(~(1 << bit_num));
		else
			desc[2].data[word_num] |= cpu_to_le32(1 << bit_num);
	}

	return 0;
}

static bool hclge_is_all_function_id_zero(struct hclge_desc *desc)
{
#define HCLGE_DESC_NUMBER 3
#define HCLGE_FUNC_NUMBER_PER_DESC 6
	int i, j;

	for (i = 1; i < HCLGE_DESC_NUMBER; i++)
		for (j = 0; j < HCLGE_FUNC_NUMBER_PER_DESC; j++)
			if (desc[i].data[j])
				return false;

	return true;
}

static void hclge_prepare_mac_addr(struct hclge_mac_vlan_tbl_entry_cmd *new_req,
				   const u8 *addr, bool is_mc)
{
	const unsigned char *mac_addr = addr;
	u32 high_val = mac_addr[2] << 16 | (mac_addr[3] << 24) |
		       (mac_addr[0]) | (mac_addr[1] << 8);
	u32 low_val  = mac_addr[4] | (mac_addr[5] << 8);

	hnae3_set_bit(new_req->flags, HCLGE_MAC_VLAN_BIT0_EN_B, 1);
	if (is_mc) {
		hnae3_set_bit(new_req->entry_type, HCLGE_MAC_VLAN_BIT1_EN_B, 1);
		hnae3_set_bit(new_req->mc_mac_en, HCLGE_MAC_VLAN_BIT0_EN_B, 1);
	}

	new_req->mac_addr_hi32 = cpu_to_le32(high_val);
	new_req->mac_addr_lo16 = cpu_to_le16(low_val & 0xffff);
}

static int hclge_remove_mac_vlan_tbl(struct hclge_vport *vport,
				     struct hclge_mac_vlan_tbl_entry_cmd *req)
{
	struct hclge_dev *hdev = vport->back;
	struct hclge_desc desc;
	u8 resp_code;
	u16 retval;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MAC_VLAN_REMOVE, false);

	memcpy(desc.data, req, sizeof(struct hclge_mac_vlan_tbl_entry_cmd));

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"del mac addr failed for cmd_send, ret =%d.\n",
			ret);
		return ret;
	}
	resp_code = (le32_to_cpu(desc.data[0]) >> 8) & 0xff;
	retval = le16_to_cpu(desc.retval);

	return hclge_get_mac_vlan_cmd_status(vport, retval, resp_code,
					     HCLGE_MAC_VLAN_REMOVE);
}

static int hclge_lookup_mac_vlan_tbl(struct hclge_vport *vport,
				     struct hclge_mac_vlan_tbl_entry_cmd *req,
				     struct hclge_desc *desc,
				     bool is_mc)
{
	struct hclge_dev *hdev = vport->back;
	u8 resp_code;
	u16 retval;
	int ret;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_MAC_VLAN_ADD, true);
	if (is_mc) {
		desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		memcpy(desc[0].data,
		       req,
		       sizeof(struct hclge_mac_vlan_tbl_entry_cmd));
		hclge_cmd_setup_basic_desc(&desc[1],
					   HCLGE_OPC_MAC_VLAN_ADD,
					   true);
		desc[1].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		hclge_cmd_setup_basic_desc(&desc[2],
					   HCLGE_OPC_MAC_VLAN_ADD,
					   true);
		ret = hclge_cmd_send(&hdev->hw, desc, 3);
	} else {
		memcpy(desc[0].data,
		       req,
		       sizeof(struct hclge_mac_vlan_tbl_entry_cmd));
		ret = hclge_cmd_send(&hdev->hw, desc, 1);
	}
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"lookup mac addr failed for cmd_send, ret =%d.\n",
			ret);
		return ret;
	}
	resp_code = (le32_to_cpu(desc[0].data[0]) >> 8) & 0xff;
	retval = le16_to_cpu(desc[0].retval);

	return hclge_get_mac_vlan_cmd_status(vport, retval, resp_code,
					     HCLGE_MAC_VLAN_LKUP);
}

static int hclge_add_mac_vlan_tbl(struct hclge_vport *vport,
				  struct hclge_mac_vlan_tbl_entry_cmd *req,
				  struct hclge_desc *mc_desc)
{
	struct hclge_dev *hdev = vport->back;
	int cfg_status;
	u8 resp_code;
	u16 retval;
	int ret;

	if (!mc_desc) {
		struct hclge_desc desc;

		hclge_cmd_setup_basic_desc(&desc,
					   HCLGE_OPC_MAC_VLAN_ADD,
					   false);
		memcpy(desc.data, req,
		       sizeof(struct hclge_mac_vlan_tbl_entry_cmd));
		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		resp_code = (le32_to_cpu(desc.data[0]) >> 8) & 0xff;
		retval = le16_to_cpu(desc.retval);

		cfg_status = hclge_get_mac_vlan_cmd_status(vport, retval,
							   resp_code,
							   HCLGE_MAC_VLAN_ADD);
	} else {
		hclge_cmd_reuse_desc(&mc_desc[0], false);
		mc_desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		hclge_cmd_reuse_desc(&mc_desc[1], false);
		mc_desc[1].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		hclge_cmd_reuse_desc(&mc_desc[2], false);
		mc_desc[2].flag &= cpu_to_le16(~HCLGE_CMD_FLAG_NEXT);
		memcpy(mc_desc[0].data, req,
		       sizeof(struct hclge_mac_vlan_tbl_entry_cmd));
		ret = hclge_cmd_send(&hdev->hw, mc_desc, 3);
		resp_code = (le32_to_cpu(mc_desc[0].data[0]) >> 8) & 0xff;
		retval = le16_to_cpu(mc_desc[0].retval);

		cfg_status = hclge_get_mac_vlan_cmd_status(vport, retval,
							   resp_code,
							   HCLGE_MAC_VLAN_ADD);
	}

	if (ret) {
		dev_err(&hdev->pdev->dev,
			"add mac addr failed for cmd_send, ret =%d.\n",
			ret);
		return ret;
	}

	return cfg_status;
}

static int hclge_init_umv_space(struct hclge_dev *hdev)
{
	u16 allocated_size = 0;
	int ret;

	ret = hclge_set_umv_space(hdev, hdev->wanted_umv_size, &allocated_size,
				  true);
	if (ret)
		return ret;

	if (allocated_size < hdev->wanted_umv_size)
		dev_warn(&hdev->pdev->dev,
			 "Alloc umv space failed, want %d, get %d\n",
			 hdev->wanted_umv_size, allocated_size);

	mutex_init(&hdev->umv_mutex);
	hdev->max_umv_size = allocated_size;
	hdev->priv_umv_size = hdev->max_umv_size / (hdev->num_req_vfs + 2);
	hdev->share_umv_size = hdev->priv_umv_size +
			hdev->max_umv_size % (hdev->num_req_vfs + 2);

	return 0;
}

static int hclge_uninit_umv_space(struct hclge_dev *hdev)
{
	int ret;

	if (hdev->max_umv_size > 0) {
		ret = hclge_set_umv_space(hdev, hdev->max_umv_size, NULL,
					  false);
		if (ret)
			return ret;
		hdev->max_umv_size = 0;
	}
	mutex_destroy(&hdev->umv_mutex);

	return 0;
}

static int hclge_set_umv_space(struct hclge_dev *hdev, u16 space_size,
			       u16 *allocated_size, bool is_alloc)
{
	struct hclge_umv_spc_alc_cmd *req;
	struct hclge_desc desc;
	int ret;

	req = (struct hclge_umv_spc_alc_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MAC_VLAN_ALLOCATE, false);
	hnae3_set_bit(req->allocate, HCLGE_UMV_SPC_ALC_B, !is_alloc);
	req->space_size = cpu_to_le32(space_size);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"%s umv space failed for cmd_send, ret =%d\n",
			is_alloc ? "allocate" : "free", ret);
		return ret;
	}

	if (is_alloc && allocated_size)
		*allocated_size = le32_to_cpu(desc.data[1]);

	return 0;
}

static void hclge_reset_umv_space(struct hclge_dev *hdev)
{
	struct hclge_vport *vport;
	int i;

	for (i = 0; i < hdev->num_alloc_vport; i++) {
		vport = &hdev->vport[i];
		vport->used_umv_num = 0;
	}

	mutex_lock(&hdev->umv_mutex);
	hdev->share_umv_size = hdev->priv_umv_size +
			hdev->max_umv_size % (hdev->num_req_vfs + 2);
	mutex_unlock(&hdev->umv_mutex);
}

static bool hclge_is_umv_space_full(struct hclge_vport *vport)
{
	struct hclge_dev *hdev = vport->back;
	bool is_full;

	mutex_lock(&hdev->umv_mutex);
	is_full = (vport->used_umv_num >= hdev->priv_umv_size &&
		   hdev->share_umv_size == 0);
	mutex_unlock(&hdev->umv_mutex);

	return is_full;
}

static void hclge_update_umv_space(struct hclge_vport *vport, bool is_free)
{
	struct hclge_dev *hdev = vport->back;

	mutex_lock(&hdev->umv_mutex);
	if (is_free) {
		if (vport->used_umv_num > hdev->priv_umv_size)
			hdev->share_umv_size++;

		if (vport->used_umv_num > 0)
			vport->used_umv_num--;
	} else {
		if (vport->used_umv_num >= hdev->priv_umv_size &&
		    hdev->share_umv_size > 0)
			hdev->share_umv_size--;
		vport->used_umv_num++;
	}
	mutex_unlock(&hdev->umv_mutex);
}

static int hclge_add_uc_addr(struct hnae3_handle *handle,
			     const unsigned char *addr)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	return hclge_add_uc_addr_common(vport, addr);
}

int hclge_add_uc_addr_common(struct hclge_vport *vport,
			     const unsigned char *addr)
{
	struct hclge_dev *hdev = vport->back;
	struct hclge_mac_vlan_tbl_entry_cmd req;
	struct hclge_desc desc;
	u16 egress_port = 0;
	int ret;

	/* mac addr check */
	if (is_zero_ether_addr(addr) ||
	    is_broadcast_ether_addr(addr) ||
	    is_multicast_ether_addr(addr)) {
		dev_err(&hdev->pdev->dev,
			"Set_uc mac err! invalid mac:%pM. is_zero:%d,is_br=%d,is_mul=%d\n",
			 addr,
			 is_zero_ether_addr(addr),
			 is_broadcast_ether_addr(addr),
			 is_multicast_ether_addr(addr));
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));

	hnae3_set_field(egress_port, HCLGE_MAC_EPORT_VFID_M,
			HCLGE_MAC_EPORT_VFID_S, vport->vport_id);

	req.egress_port = cpu_to_le16(egress_port);

	hclge_prepare_mac_addr(&req, addr, false);

	/* Lookup the mac address in the mac_vlan table, and add
	 * it if the entry is inexistent. Repeated unicast entry
	 * is not allowed in the mac vlan table.
	 */
	ret = hclge_lookup_mac_vlan_tbl(vport, &req, &desc, false);
	if (ret == -ENOENT) {
		if (!hclge_is_umv_space_full(vport)) {
			ret = hclge_add_mac_vlan_tbl(vport, &req, NULL);
			if (!ret)
				hclge_update_umv_space(vport, false);
			return ret;
		}

		dev_err(&hdev->pdev->dev, "UC MAC table full(%u)\n",
			hdev->priv_umv_size);

		return -ENOSPC;
	}

	/* check if we just hit the duplicate */
	if (!ret) {
		dev_warn(&hdev->pdev->dev, "VF %d mac(%pM) exists\n",
			 vport->vport_id, addr);
		return 0;
	}

	dev_err(&hdev->pdev->dev,
		"PF failed to add unicast entry(%pM) in the MAC table\n",
		addr);

	return ret;
}

static int hclge_rm_uc_addr(struct hnae3_handle *handle,
			    const unsigned char *addr)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	return hclge_rm_uc_addr_common(vport, addr);
}

int hclge_rm_uc_addr_common(struct hclge_vport *vport,
			    const unsigned char *addr)
{
	struct hclge_dev *hdev = vport->back;
	struct hclge_mac_vlan_tbl_entry_cmd req;
	int ret;

	/* mac addr check */
	if (is_zero_ether_addr(addr) ||
	    is_broadcast_ether_addr(addr) ||
	    is_multicast_ether_addr(addr)) {
		dev_dbg(&hdev->pdev->dev,
			"Remove mac err! invalid mac:%pM.\n",
			 addr);
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	hnae3_set_bit(req.entry_type, HCLGE_MAC_VLAN_BIT0_EN_B, 0);
	hclge_prepare_mac_addr(&req, addr, false);
	ret = hclge_remove_mac_vlan_tbl(vport, &req);
	if (!ret)
		hclge_update_umv_space(vport, true);

	return ret;
}

static int hclge_add_mc_addr(struct hnae3_handle *handle,
			     const unsigned char *addr)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	return hclge_add_mc_addr_common(vport, addr);
}

int hclge_add_mc_addr_common(struct hclge_vport *vport,
			     const unsigned char *addr)
{
	struct hclge_dev *hdev = vport->back;
	struct hclge_mac_vlan_tbl_entry_cmd req;
	struct hclge_desc desc[3];
	int status;

	/* mac addr check */
	if (!is_multicast_ether_addr(addr)) {
		dev_err(&hdev->pdev->dev,
			"Add mc mac err! invalid mac:%pM.\n",
			 addr);
		return -EINVAL;
	}
	memset(&req, 0, sizeof(req));
	hnae3_set_bit(req.entry_type, HCLGE_MAC_VLAN_BIT0_EN_B, 0);
	hclge_prepare_mac_addr(&req, addr, true);
	status = hclge_lookup_mac_vlan_tbl(vport, &req, desc, true);
	if (!status) {
		/* This mac addr exist, update VFID for it */
		hclge_update_desc_vfid(desc, vport->vport_id, false);
		status = hclge_add_mac_vlan_tbl(vport, &req, desc);
	} else {
		/* This mac addr do not exist, add new entry for it */
		memset(desc[0].data, 0, sizeof(desc[0].data));
		memset(desc[1].data, 0, sizeof(desc[0].data));
		memset(desc[2].data, 0, sizeof(desc[0].data));
		hclge_update_desc_vfid(desc, vport->vport_id, false);
		status = hclge_add_mac_vlan_tbl(vport, &req, desc);
	}

	if (status == -ENOSPC)
		dev_err(&hdev->pdev->dev, "mc mac vlan table is full\n");

	return status;
}

static int hclge_rm_mc_addr(struct hnae3_handle *handle,
			    const unsigned char *addr)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	return hclge_rm_mc_addr_common(vport, addr);
}

int hclge_rm_mc_addr_common(struct hclge_vport *vport,
			    const unsigned char *addr)
{
	struct hclge_dev *hdev = vport->back;
	struct hclge_mac_vlan_tbl_entry_cmd req;
	enum hclge_cmd_status status;
	struct hclge_desc desc[3];

	/* mac addr check */
	if (!is_multicast_ether_addr(addr)) {
		dev_dbg(&hdev->pdev->dev,
			"Remove mc mac err! invalid mac:%pM.\n",
			 addr);
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	hnae3_set_bit(req.entry_type, HCLGE_MAC_VLAN_BIT0_EN_B, 0);
	hclge_prepare_mac_addr(&req, addr, true);
	status = hclge_lookup_mac_vlan_tbl(vport, &req, desc, true);
	if (!status) {
		/* This mac addr exist, remove this handle's VFID for it */
		hclge_update_desc_vfid(desc, vport->vport_id, true);

		if (hclge_is_all_function_id_zero(desc))
			/* All the vfid is zero, so need to delete this entry */
			status = hclge_remove_mac_vlan_tbl(vport, &req);
		else
			/* Not all the vfid is zero, update the vfid */
			status = hclge_add_mac_vlan_tbl(vport, &req, desc);

	} else {
		/* Maybe this mac address is in mta table, but it cannot be
		 * deleted here because an entry of mta represents an address
		 * range rather than a specific address. the delete action to
		 * all entries will take effect in update_mta_status called by
		 * hns3_nic_set_rx_mode.
		 */
		status = 0;
	}

	return status;
}

void hclge_add_vport_mac_table(struct hclge_vport *vport, const u8 *mac_addr,
			       enum HCLGE_MAC_ADDR_TYPE mac_type)
{
	struct hclge_vport_mac_addr_cfg *mac_cfg;
	struct list_head *list;

	if (!vport->vport_id)
		return;

	mac_cfg = kzalloc(sizeof(*mac_cfg), GFP_KERNEL);
	if (!mac_cfg)
		return;

	mac_cfg->hd_tbl_status = true;
	memcpy(mac_cfg->mac_addr, mac_addr, ETH_ALEN);

	list = (mac_type == HCLGE_MAC_ADDR_UC) ?
	       &vport->uc_mac_list : &vport->mc_mac_list;

	list_add_tail(&mac_cfg->node, list);
}

void hclge_rm_vport_mac_table(struct hclge_vport *vport, const u8 *mac_addr,
			      bool is_write_tbl,
			      enum HCLGE_MAC_ADDR_TYPE mac_type)
{
	struct hclge_vport_mac_addr_cfg *mac_cfg, *tmp;
	struct list_head *list;
	bool uc_flag, mc_flag;

	list = (mac_type == HCLGE_MAC_ADDR_UC) ?
	       &vport->uc_mac_list : &vport->mc_mac_list;

	uc_flag = is_write_tbl && mac_type == HCLGE_MAC_ADDR_UC;
	mc_flag = is_write_tbl && mac_type == HCLGE_MAC_ADDR_MC;

	list_for_each_entry_safe(mac_cfg, tmp, list, node) {
		if (strncmp(mac_cfg->mac_addr, mac_addr, ETH_ALEN) == 0) {
			if (uc_flag && mac_cfg->hd_tbl_status)
				hclge_rm_uc_addr_common(vport, mac_addr);

			if (mc_flag && mac_cfg->hd_tbl_status)
				hclge_rm_mc_addr_common(vport, mac_addr);

			list_del(&mac_cfg->node);
			kfree(mac_cfg);
			break;
		}
	}
}

void hclge_rm_vport_all_mac_table(struct hclge_vport *vport, bool is_del_list,
				  enum HCLGE_MAC_ADDR_TYPE mac_type)
{
	struct hclge_vport_mac_addr_cfg *mac_cfg, *tmp;
	struct list_head *list;

	list = (mac_type == HCLGE_MAC_ADDR_UC) ?
	       &vport->uc_mac_list : &vport->mc_mac_list;

	list_for_each_entry_safe(mac_cfg, tmp, list, node) {
		if (mac_type == HCLGE_MAC_ADDR_UC && mac_cfg->hd_tbl_status)
			hclge_rm_uc_addr_common(vport, mac_cfg->mac_addr);

		if (mac_type == HCLGE_MAC_ADDR_MC && mac_cfg->hd_tbl_status)
			hclge_rm_mc_addr_common(vport, mac_cfg->mac_addr);

		mac_cfg->hd_tbl_status = false;
		if (is_del_list) {
			list_del(&mac_cfg->node);
			kfree(mac_cfg);
		}
	}
}

void hclge_uninit_vport_mac_table(struct hclge_dev *hdev)
{
	struct hclge_vport_mac_addr_cfg *mac, *tmp;
	struct hclge_vport *vport;
	int i;

	mutex_lock(&hdev->vport_cfg_mutex);
	for (i = 0; i < hdev->num_alloc_vport; i++) {
		vport = &hdev->vport[i];
		list_for_each_entry_safe(mac, tmp, &vport->uc_mac_list, node) {
			list_del(&mac->node);
			kfree(mac);
		}

		list_for_each_entry_safe(mac, tmp, &vport->mc_mac_list, node) {
			list_del(&mac->node);
			kfree(mac);
		}
	}
	mutex_unlock(&hdev->vport_cfg_mutex);
}

static int hclge_get_mac_ethertype_cmd_status(struct hclge_dev *hdev,
					      u16 cmdq_resp, u8 resp_code)
{
#define HCLGE_ETHERTYPE_SUCCESS_ADD		0
#define HCLGE_ETHERTYPE_ALREADY_ADD		1
#define HCLGE_ETHERTYPE_MGR_TBL_OVERFLOW	2
#define HCLGE_ETHERTYPE_KEY_CONFLICT		3

	int return_status;

	if (cmdq_resp) {
		dev_err(&hdev->pdev->dev,
			"cmdq execute failed for get_mac_ethertype_cmd_status, status=%d.\n",
			cmdq_resp);
		return -EIO;
	}

	switch (resp_code) {
	case HCLGE_ETHERTYPE_SUCCESS_ADD:
	case HCLGE_ETHERTYPE_ALREADY_ADD:
		return_status = 0;
		break;
	case HCLGE_ETHERTYPE_MGR_TBL_OVERFLOW:
		dev_err(&hdev->pdev->dev,
			"add mac ethertype failed for manager table overflow.\n");
		return_status = -EIO;
		break;
	case HCLGE_ETHERTYPE_KEY_CONFLICT:
		dev_err(&hdev->pdev->dev,
			"add mac ethertype failed for key conflict.\n");
		return_status = -EIO;
		break;
	default:
		dev_err(&hdev->pdev->dev,
			"add mac ethertype failed for undefined, code=%d.\n",
			resp_code);
		return_status = -EIO;
	}

	return return_status;
}

static int hclge_add_mgr_tbl(struct hclge_dev *hdev,
			     const struct hclge_mac_mgr_tbl_entry_cmd *req)
{
	struct hclge_desc desc;
	u8 resp_code;
	u16 retval;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MAC_ETHTYPE_ADD, false);
	memcpy(desc.data, req, sizeof(struct hclge_mac_mgr_tbl_entry_cmd));

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"add mac ethertype failed for cmd_send, ret =%d.\n",
			ret);
		return ret;
	}

	resp_code = (le32_to_cpu(desc.data[0]) >> 8) & 0xff;
	retval = le16_to_cpu(desc.retval);

	return hclge_get_mac_ethertype_cmd_status(hdev, retval, resp_code);
}

static int init_mgr_tbl(struct hclge_dev *hdev)
{
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(hclge_mgr_table); i++) {
		ret = hclge_add_mgr_tbl(hdev, &hclge_mgr_table[i]);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"add mac ethertype failed, ret =%d.\n",
				ret);
			return ret;
		}
	}

	return 0;
}

static void hclge_get_mac_addr(struct hnae3_handle *handle, u8 *p)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	ether_addr_copy(p, hdev->hw.mac.mac_addr);
}

static int hclge_set_mac_addr(struct hnae3_handle *handle, void *p,
			      bool is_first)
{
	const unsigned char *new_addr = (const unsigned char *)p;
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int ret;

	/* mac addr check */
	if (is_zero_ether_addr(new_addr) ||
	    is_broadcast_ether_addr(new_addr) ||
	    is_multicast_ether_addr(new_addr)) {
		dev_err(&hdev->pdev->dev,
			"Change uc mac err! invalid mac:%p.\n",
			 new_addr);
		return -EINVAL;
	}

	if (!is_first && hclge_rm_uc_addr(handle, hdev->hw.mac.mac_addr))
		dev_warn(&hdev->pdev->dev,
			 "remove old uc mac address fail.\n");

	ret = hclge_add_uc_addr(handle, new_addr);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"add uc mac address fail, ret =%d.\n",
			ret);

		if (!is_first &&
		    hclge_add_uc_addr(handle, hdev->hw.mac.mac_addr))
			dev_err(&hdev->pdev->dev,
				"restore uc mac address fail.\n");

		return -EIO;
	}

	ret = hclge_pause_addr_cfg(hdev, new_addr);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"configure mac pause address fail, ret =%d.\n",
			ret);
		return -EIO;
	}

	ether_addr_copy(hdev->hw.mac.mac_addr, new_addr);

	return 0;
}

static int hclge_do_ioctl(struct hnae3_handle *handle, struct ifreq *ifr,
			  int cmd)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (!hdev->hw.mac.phydev)
		return -EOPNOTSUPP;

	return phy_mii_ioctl(hdev->hw.mac.phydev, ifr, cmd);
}

static int hclge_set_vlan_filter_ctrl(struct hclge_dev *hdev, u8 vlan_type,
				      u8 fe_type, bool filter_en, u8 vf_id)
{
	struct hclge_vlan_filter_ctrl_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_VLAN_FILTER_CTRL, false);

	req = (struct hclge_vlan_filter_ctrl_cmd *)desc.data;
	req->vlan_type = vlan_type;
	req->vlan_fe = filter_en ? fe_type : 0;
	req->vf_id = vf_id;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev, "set vlan filter fail, ret =%d.\n",
			ret);

	return ret;
}

#define HCLGE_FILTER_TYPE_VF		0
#define HCLGE_FILTER_TYPE_PORT		1
#define HCLGE_FILTER_FE_EGRESS_V1_B	BIT(0)
#define HCLGE_FILTER_FE_NIC_INGRESS_B	BIT(0)
#define HCLGE_FILTER_FE_NIC_EGRESS_B	BIT(1)
#define HCLGE_FILTER_FE_ROCE_INGRESS_B	BIT(2)
#define HCLGE_FILTER_FE_ROCE_EGRESS_B	BIT(3)
#define HCLGE_FILTER_FE_EGRESS		(HCLGE_FILTER_FE_NIC_EGRESS_B \
					| HCLGE_FILTER_FE_ROCE_EGRESS_B)
#define HCLGE_FILTER_FE_INGRESS		(HCLGE_FILTER_FE_NIC_INGRESS_B \
					| HCLGE_FILTER_FE_ROCE_INGRESS_B)

static void hclge_enable_vlan_filter(struct hnae3_handle *handle, bool enable)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (hdev->pdev->revision >= 0x21) {
		hclge_set_vlan_filter_ctrl(hdev, HCLGE_FILTER_TYPE_VF,
					   HCLGE_FILTER_FE_EGRESS, enable, 0);
		hclge_set_vlan_filter_ctrl(hdev, HCLGE_FILTER_TYPE_PORT,
					   HCLGE_FILTER_FE_INGRESS, enable, 0);
	} else {
		hclge_set_vlan_filter_ctrl(hdev, HCLGE_FILTER_TYPE_VF,
					   HCLGE_FILTER_FE_EGRESS_V1_B, enable,
					   0);
	}
	if (enable)
		handle->netdev_flags |= HNAE3_VLAN_FLTR;
	else
		handle->netdev_flags &= ~HNAE3_VLAN_FLTR;
}

static int hclge_set_vf_vlan_common(struct hclge_dev *hdev, int vfid,
				    bool is_kill, u16 vlan, u8 qos,
				    __be16 proto)
{
#define HCLGE_MAX_VF_BYTES  16
	struct hclge_vlan_filter_vf_cfg_cmd *req0;
	struct hclge_vlan_filter_vf_cfg_cmd *req1;
	struct hclge_desc desc[2];
	u8 vf_byte_val;
	u8 vf_byte_off;
	int ret;

	hclge_cmd_setup_basic_desc(&desc[0],
				   HCLGE_OPC_VLAN_FILTER_VF_CFG, false);
	hclge_cmd_setup_basic_desc(&desc[1],
				   HCLGE_OPC_VLAN_FILTER_VF_CFG, false);

	desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);

	vf_byte_off = vfid / 8;
	vf_byte_val = 1 << (vfid % 8);

	req0 = (struct hclge_vlan_filter_vf_cfg_cmd *)desc[0].data;
	req1 = (struct hclge_vlan_filter_vf_cfg_cmd *)desc[1].data;

	req0->vlan_id  = cpu_to_le16(vlan);
	req0->vlan_cfg = is_kill;

	if (vf_byte_off < HCLGE_MAX_VF_BYTES)
		req0->vf_bitmap[vf_byte_off] = vf_byte_val;
	else
		req1->vf_bitmap[vf_byte_off - HCLGE_MAX_VF_BYTES] = vf_byte_val;

	ret = hclge_cmd_send(&hdev->hw, desc, 2);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Send vf vlan command fail, ret =%d.\n",
			ret);
		return ret;
	}

	if (!is_kill) {
#define HCLGE_VF_VLAN_NO_ENTRY	2
		if (!req0->resp_code || req0->resp_code == 1)
			return 0;

		if (req0->resp_code == HCLGE_VF_VLAN_NO_ENTRY) {
			dev_warn(&hdev->pdev->dev,
				 "vf vlan table is full, vf vlan filter is disabled\n");
			return 0;
		}

		dev_err(&hdev->pdev->dev,
			"Add vf vlan filter fail, ret =%d.\n",
			req0->resp_code);
	} else {
#define HCLGE_VF_VLAN_DEL_NO_FOUND	1
		if (!req0->resp_code)
			return 0;

		if (req0->resp_code == HCLGE_VF_VLAN_DEL_NO_FOUND) {
			dev_warn(&hdev->pdev->dev,
				 "vlan %d filter is not in vf vlan table\n",
				 vlan);
			return 0;
		}

		dev_err(&hdev->pdev->dev,
			"Kill vf vlan filter fail, ret =%d.\n",
			req0->resp_code);
	}

	return -EIO;
}

static int hclge_set_port_vlan_filter(struct hclge_dev *hdev, __be16 proto,
				      u16 vlan_id, bool is_kill)
{
	struct hclge_vlan_filter_pf_cfg_cmd *req;
	struct hclge_desc desc;
	u8 vlan_offset_byte_val;
	u8 vlan_offset_byte;
	u8 vlan_offset_160;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_VLAN_FILTER_PF_CFG, false);

	vlan_offset_160 = vlan_id / 160;
	vlan_offset_byte = (vlan_id % 160) / 8;
	vlan_offset_byte_val = 1 << (vlan_id % 8);

	req = (struct hclge_vlan_filter_pf_cfg_cmd *)desc.data;
	req->vlan_offset = vlan_offset_160;
	req->vlan_cfg = is_kill;
	req->vlan_offset_bitmap[vlan_offset_byte] = vlan_offset_byte_val;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"port vlan command, send fail, ret =%d.\n", ret);
	return ret;
}

static int hclge_set_vlan_filter_hw(struct hclge_dev *hdev, __be16 proto,
				    u16 vport_id, u16 vlan_id, u8 qos,
				    bool is_kill)
{
	u16 vport_idx, vport_num = 0;
	int ret;

	if (is_kill && !vlan_id)
		return 0;

	ret = hclge_set_vf_vlan_common(hdev, vport_id, is_kill, vlan_id,
				       0, proto);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Set %d vport vlan filter config fail, ret =%d.\n",
			vport_id, ret);
		return ret;
	}

	/* vlan 0 may be added twice when 8021q module is enabled */
	if (!is_kill && !vlan_id &&
	    test_bit(vport_id, hdev->vlan_table[vlan_id]))
		return 0;

	if (!is_kill && test_and_set_bit(vport_id, hdev->vlan_table[vlan_id])) {
		dev_err(&hdev->pdev->dev,
			"Add port vlan failed, vport %d is already in vlan %d\n",
			vport_id, vlan_id);
		return -EINVAL;
	}

	if (is_kill &&
	    !test_and_clear_bit(vport_id, hdev->vlan_table[vlan_id])) {
		dev_err(&hdev->pdev->dev,
			"Delete port vlan failed, vport %d is not in vlan %d\n",
			vport_id, vlan_id);
		return -EINVAL;
	}

	for_each_set_bit(vport_idx, hdev->vlan_table[vlan_id], HCLGE_VPORT_NUM)
		vport_num++;

	if ((is_kill && vport_num == 0) || (!is_kill && vport_num == 1))
		ret = hclge_set_port_vlan_filter(hdev, proto, vlan_id,
						 is_kill);

	return ret;
}

int hclge_set_vlan_filter(struct hnae3_handle *handle, __be16 proto,
			  u16 vlan_id, bool is_kill)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hclge_set_vlan_filter_hw(hdev, proto, vport->vport_id, vlan_id,
					0, is_kill);
}

static int hclge_set_vf_vlan_filter(struct hnae3_handle *handle, int vfid,
				    u16 vlan, u8 qos, __be16 proto)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if ((vfid >= hdev->num_alloc_vfs) || (vlan > 4095) || (qos > 7))
		return -EINVAL;
	if (proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;

	return hclge_set_vlan_filter_hw(hdev, proto, vfid, vlan, qos, false);
}

static int hclge_set_vlan_tx_offload_cfg(struct hclge_vport *vport)
{
	struct hclge_tx_vtag_cfg *vcfg = &vport->txvlan_cfg;
	struct hclge_vport_vtag_tx_cfg_cmd *req;
	struct hclge_dev *hdev = vport->back;
	struct hclge_desc desc;
	int status;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_VLAN_PORT_TX_CFG, false);

	req = (struct hclge_vport_vtag_tx_cfg_cmd *)desc.data;
	req->def_vlan_tag1 = cpu_to_le16(vcfg->default_tag1);
	req->def_vlan_tag2 = cpu_to_le16(vcfg->default_tag2);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_ACCEPT_TAG1_B,
		      vcfg->accept_tag1 ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_ACCEPT_UNTAG1_B,
		      vcfg->accept_untag1 ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_ACCEPT_TAG2_B,
		      vcfg->accept_tag2 ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_ACCEPT_UNTAG2_B,
		      vcfg->accept_untag2 ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_PORT_INS_TAG1_EN_B,
		      vcfg->insert_tag1_en ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_PORT_INS_TAG2_EN_B,
		      vcfg->insert_tag2_en ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_CFG_NIC_ROCE_SEL_B, 0);

	req->vf_offset = vport->vport_id / HCLGE_VF_NUM_PER_CMD;
	req->vf_bitmap[req->vf_offset] =
		1 << (vport->vport_id % HCLGE_VF_NUM_PER_BYTE);

	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status)
		dev_err(&hdev->pdev->dev,
			"Send port txvlan cfg command fail, ret =%d\n",
			status);

	return status;
}

static int hclge_set_vlan_rx_offload_cfg(struct hclge_vport *vport)
{
	struct hclge_rx_vtag_cfg *vcfg = &vport->rxvlan_cfg;
	struct hclge_vport_vtag_rx_cfg_cmd *req;
	struct hclge_dev *hdev = vport->back;
	struct hclge_desc desc;
	int status;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_VLAN_PORT_RX_CFG, false);

	req = (struct hclge_vport_vtag_rx_cfg_cmd *)desc.data;
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_REM_TAG1_EN_B,
		      vcfg->strip_tag1_en ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_REM_TAG2_EN_B,
		      vcfg->strip_tag2_en ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_SHOW_TAG1_EN_B,
		      vcfg->vlan1_vlan_prionly ? 1 : 0);
	hnae3_set_bit(req->vport_vlan_cfg, HCLGE_SHOW_TAG2_EN_B,
		      vcfg->vlan2_vlan_prionly ? 1 : 0);

	req->vf_offset = vport->vport_id / HCLGE_VF_NUM_PER_CMD;
	req->vf_bitmap[req->vf_offset] =
		1 << (vport->vport_id % HCLGE_VF_NUM_PER_BYTE);

	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status)
		dev_err(&hdev->pdev->dev,
			"Send port rxvlan cfg command fail, ret =%d\n",
			status);

	return status;
}

static int hclge_set_vlan_protocol_type(struct hclge_dev *hdev)
{
	struct hclge_rx_vlan_type_cfg_cmd *rx_req;
	struct hclge_tx_vlan_type_cfg_cmd *tx_req;
	struct hclge_desc desc;
	int status;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MAC_VLAN_TYPE_ID, false);
	rx_req = (struct hclge_rx_vlan_type_cfg_cmd *)desc.data;
	rx_req->ot_fst_vlan_type =
		cpu_to_le16(hdev->vlan_type_cfg.rx_ot_fst_vlan_type);
	rx_req->ot_sec_vlan_type =
		cpu_to_le16(hdev->vlan_type_cfg.rx_ot_sec_vlan_type);
	rx_req->in_fst_vlan_type =
		cpu_to_le16(hdev->vlan_type_cfg.rx_in_fst_vlan_type);
	rx_req->in_sec_vlan_type =
		cpu_to_le16(hdev->vlan_type_cfg.rx_in_sec_vlan_type);

	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status) {
		dev_err(&hdev->pdev->dev,
			"Send rxvlan protocol type command fail, ret =%d\n",
			status);
		return status;
	}

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MAC_VLAN_INSERT, false);

	tx_req = (struct hclge_tx_vlan_type_cfg_cmd *)desc.data;
	tx_req->ot_vlan_type = cpu_to_le16(hdev->vlan_type_cfg.tx_ot_vlan_type);
	tx_req->in_vlan_type = cpu_to_le16(hdev->vlan_type_cfg.tx_in_vlan_type);

	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status)
		dev_err(&hdev->pdev->dev,
			"Send txvlan protocol type command fail, ret =%d\n",
			status);

	return status;
}

static int hclge_init_vlan_config(struct hclge_dev *hdev)
{
#define HCLGE_DEF_VLAN_TYPE		0x8100

	struct hnae3_handle *handle = &hdev->vport[0].nic;
	struct hclge_vport *vport;
	int ret;
	int i;

	if (hdev->pdev->revision >= 0x21) {
		/* for revision 0x21, vf vlan filter is per function */
		for (i = 0; i < hdev->num_alloc_vport; i++) {
			vport = &hdev->vport[i];
			ret = hclge_set_vlan_filter_ctrl(hdev,
							 HCLGE_FILTER_TYPE_VF,
							 HCLGE_FILTER_FE_EGRESS,
							 true,
							 vport->vport_id);
			if (ret)
				return ret;
		}

		ret = hclge_set_vlan_filter_ctrl(hdev, HCLGE_FILTER_TYPE_PORT,
						 HCLGE_FILTER_FE_INGRESS, true,
						 0);
		if (ret)
			return ret;
	} else {
		ret = hclge_set_vlan_filter_ctrl(hdev, HCLGE_FILTER_TYPE_VF,
						 HCLGE_FILTER_FE_EGRESS_V1_B,
						 true, 0);
		if (ret)
			return ret;
	}

	handle->netdev_flags |= HNAE3_VLAN_FLTR;

	hdev->vlan_type_cfg.rx_in_fst_vlan_type = HCLGE_DEF_VLAN_TYPE;
	hdev->vlan_type_cfg.rx_in_sec_vlan_type = HCLGE_DEF_VLAN_TYPE;
	hdev->vlan_type_cfg.rx_ot_fst_vlan_type = HCLGE_DEF_VLAN_TYPE;
	hdev->vlan_type_cfg.rx_ot_sec_vlan_type = HCLGE_DEF_VLAN_TYPE;
	hdev->vlan_type_cfg.tx_ot_vlan_type = HCLGE_DEF_VLAN_TYPE;
	hdev->vlan_type_cfg.tx_in_vlan_type = HCLGE_DEF_VLAN_TYPE;

	ret = hclge_set_vlan_protocol_type(hdev);
	if (ret)
		return ret;

	for (i = 0; i < hdev->num_alloc_vport; i++) {
		vport = &hdev->vport[i];
		vport->txvlan_cfg.accept_tag1 = true;
		vport->txvlan_cfg.accept_untag1 = true;

		/* accept_tag2 and accept_untag2 are not supported on
		 * pdev revision(0x20), new revision support them. The
		 * value of this two fields will not return error when driver
		 * send command to fireware in revision(0x20).
		 * This two fields can not configured by user.
		 */
		vport->txvlan_cfg.accept_tag2 = true;
		vport->txvlan_cfg.accept_untag2 = true;

		vport->txvlan_cfg.insert_tag1_en = false;
		vport->txvlan_cfg.insert_tag2_en = false;
		vport->txvlan_cfg.default_tag1 = 0;
		vport->txvlan_cfg.default_tag2 = 0;

		ret = hclge_set_vlan_tx_offload_cfg(vport);
		if (ret)
			return ret;

		vport->rxvlan_cfg.strip_tag1_en = false;
		vport->rxvlan_cfg.strip_tag2_en = true;
		vport->rxvlan_cfg.vlan1_vlan_prionly = false;
		vport->rxvlan_cfg.vlan2_vlan_prionly = false;

		ret = hclge_set_vlan_rx_offload_cfg(vport);
		if (ret)
			return ret;
	}

	return hclge_set_vlan_filter(handle, htons(ETH_P_8021Q), 0, false);
}

void hclge_add_vport_vlan_table(struct hclge_vport *vport, u16 vlan_id)
{
	struct hclge_vport_vlan_cfg *vlan;

	/* vlan 0 is reserved */
	if (!vlan_id)
		return;

	vlan = kzalloc(sizeof(*vlan), GFP_KERNEL);
	if (!vlan)
		return;

	vlan->hd_tbl_status = true;
	vlan->vlan_id = vlan_id;

	list_add_tail(&vlan->node, &vport->vlan_list);
}

void hclge_rm_vport_vlan_table(struct hclge_vport *vport, u16 vlan_id,
			       bool is_write_tbl)
{
	struct hclge_vport_vlan_cfg *vlan, *tmp;
	struct hclge_dev *hdev = vport->back;

	list_for_each_entry_safe(vlan, tmp, &vport->vlan_list, node) {
		if (vlan->vlan_id == vlan_id) {
			if (is_write_tbl && vlan->hd_tbl_status)
				hclge_set_vlan_filter_hw(hdev,
							 htons(ETH_P_8021Q),
							 vport->vport_id,
							 vlan_id, 0,
							 true);

			list_del(&vlan->node);
			kfree(vlan);
			break;
		}
	}
}

void hclge_rm_vport_all_vlan_table(struct hclge_vport *vport, bool is_del_list)
{
	struct hclge_vport_vlan_cfg *vlan, *tmp;
	struct hclge_dev *hdev = vport->back;

	list_for_each_entry_safe(vlan, tmp, &vport->vlan_list, node) {
		if (vlan->hd_tbl_status)
			hclge_set_vlan_filter_hw(hdev,
						 htons(ETH_P_8021Q),
						 vport->vport_id,
						 vlan->vlan_id, 0,
						 true);

		vlan->hd_tbl_status = false;
		if (is_del_list) {
			list_del(&vlan->node);
			kfree(vlan);
		}
	}
}

void hclge_uninit_vport_vlan_table(struct hclge_dev *hdev)
{
	struct hclge_vport_vlan_cfg *vlan, *tmp;
	struct hclge_vport *vport;
	int i;

	mutex_lock(&hdev->vport_cfg_mutex);
	for (i = 0; i < hdev->num_alloc_vport; i++) {
		vport = &hdev->vport[i];
		list_for_each_entry_safe(vlan, tmp, &vport->vlan_list, node) {
			list_del(&vlan->node);
			kfree(vlan);
		}
	}
	mutex_unlock(&hdev->vport_cfg_mutex);
}

int hclge_en_hw_strip_rxvtag(struct hnae3_handle *handle, bool enable)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	vport->rxvlan_cfg.strip_tag1_en = false;
	vport->rxvlan_cfg.strip_tag2_en = enable;
	vport->rxvlan_cfg.vlan1_vlan_prionly = false;
	vport->rxvlan_cfg.vlan2_vlan_prionly = false;

	return hclge_set_vlan_rx_offload_cfg(vport);
}

static int hclge_set_mac_mtu(struct hclge_dev *hdev, int new_mps)
{
	struct hclge_config_max_frm_size_cmd *req;
	struct hclge_desc desc;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CONFIG_MAX_FRM_SIZE, false);

	req = (struct hclge_config_max_frm_size_cmd *)desc.data;
	req->max_frm_size = cpu_to_le16(new_mps);
	req->min_frm_size = HCLGE_MAC_MIN_FRAME;

	return hclge_cmd_send(&hdev->hw, &desc, 1);
}

static int hclge_set_mtu(struct hnae3_handle *handle, int new_mtu)
{
	struct hclge_vport *vport = hclge_get_vport(handle);

	return hclge_set_vport_mtu(vport, new_mtu);
}

int hclge_set_vport_mtu(struct hclge_vport *vport, int new_mtu)
{
	struct hclge_dev *hdev = vport->back;
	int i, max_frm_size, ret = 0;

	max_frm_size = new_mtu + ETH_HLEN + ETH_FCS_LEN + 2 * VLAN_HLEN;
	if (max_frm_size < HCLGE_MAC_MIN_FRAME ||
	    max_frm_size > HCLGE_MAC_MAX_FRAME)
		return -EINVAL;

	max_frm_size = max(max_frm_size, HCLGE_MAC_DEFAULT_FRAME);
	mutex_lock(&hdev->vport_lock);
	/* VF's mps must fit within hdev->mps */
	if (vport->vport_id && max_frm_size > hdev->mps) {
		mutex_unlock(&hdev->vport_lock);
		return -EINVAL;
	} else if (vport->vport_id) {
		vport->mps = max_frm_size;
		mutex_unlock(&hdev->vport_lock);
		return 0;
	}

	/* PF's mps must be greater then VF's mps */
	for (i = 1; i < hdev->num_alloc_vport; i++)
		if (max_frm_size < hdev->vport[i].mps) {
			mutex_unlock(&hdev->vport_lock);
			return -EINVAL;
		}

	hclge_notify_client(hdev, HNAE3_DOWN_CLIENT);

	ret = hclge_set_mac_mtu(hdev, max_frm_size);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Change mtu fail, ret =%d\n", ret);
		goto out;
	}

	hdev->mps = max_frm_size;
	vport->mps = max_frm_size;

	ret = hclge_buffer_alloc(hdev);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Allocate buffer fail, ret =%d\n", ret);

out:
	hclge_notify_client(hdev, HNAE3_UP_CLIENT);
	mutex_unlock(&hdev->vport_lock);
	return ret;
}

static int hclge_send_reset_tqp_cmd(struct hclge_dev *hdev, u16 queue_id,
				    bool enable)
{
	struct hclge_reset_tqp_queue_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RESET_TQP_QUEUE, false);

	req = (struct hclge_reset_tqp_queue_cmd *)desc.data;
	req->tqp_id = cpu_to_le16(queue_id & HCLGE_RING_ID_MASK);
	hnae3_set_bit(req->reset_req, HCLGE_TQP_RESET_B, enable);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Send tqp reset cmd error, status =%d\n", ret);
		return ret;
	}

	return 0;
}

static int hclge_get_reset_status(struct hclge_dev *hdev, u16 queue_id)
{
	struct hclge_reset_tqp_queue_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_RESET_TQP_QUEUE, true);

	req = (struct hclge_reset_tqp_queue_cmd *)desc.data;
	req->tqp_id = cpu_to_le16(queue_id & HCLGE_RING_ID_MASK);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Get reset status error, status =%d\n", ret);
		return ret;
	}

	return hnae3_get_bit(req->ready_to_reset, HCLGE_TQP_RESET_B);
}

u16 hclge_covert_handle_qid_global(struct hnae3_handle *handle, u16 queue_id)
{
	struct hnae3_queue *queue;
	struct hclge_tqp *tqp;

	queue = handle->kinfo.tqp[queue_id];
	tqp = container_of(queue, struct hclge_tqp, q);

	return tqp->index;
}

int hclge_reset_tqp(struct hnae3_handle *handle, u16 queue_id)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	int reset_try_times = 0;
	int reset_status;
	u16 queue_gid;
	int ret = 0;

	queue_gid = hclge_covert_handle_qid_global(handle, queue_id);

	ret = hclge_tqp_enable(hdev, queue_id, 0, false);
	if (ret) {
		dev_err(&hdev->pdev->dev, "Disable tqp fail, ret = %d\n", ret);
		return ret;
	}

	ret = hclge_send_reset_tqp_cmd(hdev, queue_gid, true);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Send reset tqp cmd fail, ret = %d\n", ret);
		return ret;
	}

	reset_try_times = 0;
	while (reset_try_times++ < HCLGE_TQP_RESET_TRY_TIMES) {
		/* Wait for tqp hw reset */
		msleep(20);
		reset_status = hclge_get_reset_status(hdev, queue_gid);
		if (reset_status)
			break;
	}

	if (reset_try_times >= HCLGE_TQP_RESET_TRY_TIMES) {
		dev_err(&hdev->pdev->dev, "Reset TQP fail\n");
		return ret;
	}

	ret = hclge_send_reset_tqp_cmd(hdev, queue_gid, false);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Deassert the soft reset fail, ret = %d\n", ret);

	return ret;
}

void hclge_reset_vf_queue(struct hclge_vport *vport, u16 queue_id)
{
	struct hclge_dev *hdev = vport->back;
	int reset_try_times = 0;
	int reset_status;
	u16 queue_gid;
	int ret;

	queue_gid = hclge_covert_handle_qid_global(&vport->nic, queue_id);

	ret = hclge_send_reset_tqp_cmd(hdev, queue_gid, true);
	if (ret) {
		dev_warn(&hdev->pdev->dev,
			 "Send reset tqp cmd fail, ret = %d\n", ret);
		return;
	}

	reset_try_times = 0;
	while (reset_try_times++ < HCLGE_TQP_RESET_TRY_TIMES) {
		/* Wait for tqp hw reset */
		msleep(20);
		reset_status = hclge_get_reset_status(hdev, queue_gid);
		if (reset_status)
			break;
	}

	if (reset_try_times >= HCLGE_TQP_RESET_TRY_TIMES) {
		dev_warn(&hdev->pdev->dev, "Reset TQP fail\n");
		return;
	}

	ret = hclge_send_reset_tqp_cmd(hdev, queue_gid, false);
	if (ret)
		dev_warn(&hdev->pdev->dev,
			 "Deassert the soft reset fail, ret = %d\n", ret);
}

static u32 hclge_get_fw_version(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hdev->fw_version;
}

static void hclge_set_flowctrl_adv(struct hclge_dev *hdev, u32 rx_en, u32 tx_en)
{
	struct phy_device *phydev = hdev->hw.mac.phydev;

	if (!phydev)
		return;

	phy_set_asym_pause(phydev, rx_en, tx_en);
}

static int hclge_cfg_pauseparam(struct hclge_dev *hdev, u32 rx_en, u32 tx_en)
{
	int ret;

	if (rx_en && tx_en)
		hdev->fc_mode_last_time = HCLGE_FC_FULL;
	else if (rx_en && !tx_en)
		hdev->fc_mode_last_time = HCLGE_FC_RX_PAUSE;
	else if (!rx_en && tx_en)
		hdev->fc_mode_last_time = HCLGE_FC_TX_PAUSE;
	else
		hdev->fc_mode_last_time = HCLGE_FC_NONE;

	if (hdev->tm_info.fc_mode == HCLGE_FC_PFC)
		return 0;

	ret = hclge_mac_pause_en_cfg(hdev, tx_en, rx_en);
	if (ret) {
		dev_err(&hdev->pdev->dev, "configure pauseparam error, ret = %d.\n",
			ret);
		return ret;
	}

	hdev->tm_info.fc_mode = hdev->fc_mode_last_time;

	return 0;
}

int hclge_cfg_flowctrl(struct hclge_dev *hdev)
{
	struct phy_device *phydev = hdev->hw.mac.phydev;
	u16 remote_advertising = 0;
	u16 local_advertising = 0;
	u32 rx_pause, tx_pause;
	u8 flowctl;

	if (!phydev->link || !phydev->autoneg)
		return 0;

	local_advertising = linkmode_adv_to_lcl_adv_t(phydev->advertising);

	if (phydev->pause)
		remote_advertising = LPA_PAUSE_CAP;

	if (phydev->asym_pause)
		remote_advertising |= LPA_PAUSE_ASYM;

	flowctl = mii_resolve_flowctrl_fdx(local_advertising,
					   remote_advertising);
	tx_pause = flowctl & FLOW_CTRL_TX;
	rx_pause = flowctl & FLOW_CTRL_RX;

	if (phydev->duplex == HCLGE_MAC_HALF) {
		tx_pause = 0;
		rx_pause = 0;
	}

	return hclge_cfg_pauseparam(hdev, rx_pause, tx_pause);
}

static void hclge_get_pauseparam(struct hnae3_handle *handle, u32 *auto_neg,
				 u32 *rx_en, u32 *tx_en)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	*auto_neg = hclge_get_autoneg(handle);

	if (hdev->tm_info.fc_mode == HCLGE_FC_PFC) {
		*rx_en = 0;
		*tx_en = 0;
		return;
	}

	if (hdev->tm_info.fc_mode == HCLGE_FC_RX_PAUSE) {
		*rx_en = 1;
		*tx_en = 0;
	} else if (hdev->tm_info.fc_mode == HCLGE_FC_TX_PAUSE) {
		*tx_en = 1;
		*rx_en = 0;
	} else if (hdev->tm_info.fc_mode == HCLGE_FC_FULL) {
		*rx_en = 1;
		*tx_en = 1;
	} else {
		*rx_en = 0;
		*tx_en = 0;
	}
}

static int hclge_set_pauseparam(struct hnae3_handle *handle, u32 auto_neg,
				u32 rx_en, u32 tx_en)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct phy_device *phydev = hdev->hw.mac.phydev;
	u32 fc_autoneg;

	fc_autoneg = hclge_get_autoneg(handle);
	if (auto_neg != fc_autoneg) {
		dev_info(&hdev->pdev->dev,
			 "To change autoneg please use: ethtool -s <dev> autoneg <on|off>\n");
		return -EOPNOTSUPP;
	}

	if (hdev->tm_info.fc_mode == HCLGE_FC_PFC) {
		dev_info(&hdev->pdev->dev,
			 "Priority flow control enabled. Cannot set link flow control.\n");
		return -EOPNOTSUPP;
	}

	hclge_set_flowctrl_adv(hdev, rx_en, tx_en);

	if (!fc_autoneg)
		return hclge_cfg_pauseparam(hdev, rx_en, tx_en);

	/* Only support flow control negotiation for netdev with
	 * phy attached for now.
	 */
	if (!phydev)
		return -EOPNOTSUPP;

	return phy_start_aneg(phydev);
}

static void hclge_get_ksettings_an_result(struct hnae3_handle *handle,
					  u8 *auto_neg, u32 *speed, u8 *duplex)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (speed)
		*speed = hdev->hw.mac.speed;
	if (duplex)
		*duplex = hdev->hw.mac.duplex;
	if (auto_neg)
		*auto_neg = hdev->hw.mac.autoneg;
}

static void hclge_get_media_type(struct hnae3_handle *handle, u8 *media_type)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (media_type)
		*media_type = hdev->hw.mac.media_type;
}

static void hclge_get_mdix_mode(struct hnae3_handle *handle,
				u8 *tp_mdix_ctrl, u8 *tp_mdix)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct phy_device *phydev = hdev->hw.mac.phydev;
	int mdix_ctrl, mdix, retval, is_resolved;

	if (!phydev) {
		*tp_mdix_ctrl = ETH_TP_MDI_INVALID;
		*tp_mdix = ETH_TP_MDI_INVALID;
		return;
	}

	phy_write(phydev, HCLGE_PHY_PAGE_REG, HCLGE_PHY_PAGE_MDIX);

	retval = phy_read(phydev, HCLGE_PHY_CSC_REG);
	mdix_ctrl = hnae3_get_field(retval, HCLGE_PHY_MDIX_CTRL_M,
				    HCLGE_PHY_MDIX_CTRL_S);

	retval = phy_read(phydev, HCLGE_PHY_CSS_REG);
	mdix = hnae3_get_bit(retval, HCLGE_PHY_MDIX_STATUS_B);
	is_resolved = hnae3_get_bit(retval, HCLGE_PHY_SPEED_DUP_RESOLVE_B);

	phy_write(phydev, HCLGE_PHY_PAGE_REG, HCLGE_PHY_PAGE_COPPER);

	switch (mdix_ctrl) {
	case 0x0:
		*tp_mdix_ctrl = ETH_TP_MDI;
		break;
	case 0x1:
		*tp_mdix_ctrl = ETH_TP_MDI_X;
		break;
	case 0x3:
		*tp_mdix_ctrl = ETH_TP_MDI_AUTO;
		break;
	default:
		*tp_mdix_ctrl = ETH_TP_MDI_INVALID;
		break;
	}

	if (!is_resolved)
		*tp_mdix = ETH_TP_MDI_INVALID;
	else if (mdix)
		*tp_mdix = ETH_TP_MDI_X;
	else
		*tp_mdix = ETH_TP_MDI;
}

static int hclge_init_client_instance(struct hnae3_client *client,
				      struct hnae3_ae_dev *ae_dev)
{
	struct hclge_dev *hdev = ae_dev->priv;
	struct hclge_vport *vport;
	int i, ret;

	for (i = 0; i <  hdev->num_vmdq_vport + 1; i++) {
		vport = &hdev->vport[i];

		switch (client->type) {
		case HNAE3_CLIENT_KNIC:

			hdev->nic_client = client;
			vport->nic.client = client;
			ret = client->ops->init_instance(&vport->nic);
			if (ret)
				goto clear_nic;

			hnae3_set_client_init_flag(client, ae_dev, 1);

			if (hdev->roce_client &&
			    hnae3_dev_roce_supported(hdev)) {
				struct hnae3_client *rc = hdev->roce_client;

				ret = hclge_init_roce_base_info(vport);
				if (ret)
					goto clear_roce;

				ret = rc->ops->init_instance(&vport->roce);
				if (ret)
					goto clear_roce;

				hnae3_set_client_init_flag(hdev->roce_client,
							   ae_dev, 1);
			}

			break;
		case HNAE3_CLIENT_UNIC:
			hdev->nic_client = client;
			vport->nic.client = client;

			ret = client->ops->init_instance(&vport->nic);
			if (ret)
				goto clear_nic;

			hnae3_set_client_init_flag(client, ae_dev, 1);

			break;
		case HNAE3_CLIENT_ROCE:
			if (hnae3_dev_roce_supported(hdev)) {
				hdev->roce_client = client;
				vport->roce.client = client;
			}

			if (hdev->roce_client && hdev->nic_client) {
				ret = hclge_init_roce_base_info(vport);
				if (ret)
					goto clear_roce;

				ret = client->ops->init_instance(&vport->roce);
				if (ret)
					goto clear_roce;

				hnae3_set_client_init_flag(client, ae_dev, 1);
			}

			break;
		default:
			return -EINVAL;
		}
	}

	return 0;

clear_nic:
	hdev->nic_client = NULL;
	vport->nic.client = NULL;
	return ret;
clear_roce:
	hdev->roce_client = NULL;
	vport->roce.client = NULL;
	return ret;
}

static void hclge_uninit_client_instance(struct hnae3_client *client,
					 struct hnae3_ae_dev *ae_dev)
{
	struct hclge_dev *hdev = ae_dev->priv;
	struct hclge_vport *vport;
	int i;

	for (i = 0; i < hdev->num_vmdq_vport + 1; i++) {
		vport = &hdev->vport[i];
		if (hdev->roce_client) {
			hdev->roce_client->ops->uninit_instance(&vport->roce,
								0);
			hdev->roce_client = NULL;
			vport->roce.client = NULL;
		}
		if (client->type == HNAE3_CLIENT_ROCE)
			return;
		if (hdev->nic_client && client->ops->uninit_instance) {
			client->ops->uninit_instance(&vport->nic, 0);
			hdev->nic_client = NULL;
			vport->nic.client = NULL;
		}
	}
}

static int hclge_pci_init(struct hclge_dev *hdev)
{
	struct pci_dev *pdev = hdev->pdev;
	struct hclge_hw *hw;
	int ret;

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device\n");
		return ret;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev,
				"can't set consistent PCI DMA");
			goto err_disable_device;
		}
		dev_warn(&pdev->dev, "set DMA mask to 32 bits\n");
	}

	ret = pci_request_regions(pdev, HCLGE_DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "PCI request regions failed %d\n", ret);
		goto err_disable_device;
	}

	pci_set_master(pdev);
	hw = &hdev->hw;
	hw->io_base = pcim_iomap(pdev, 2, 0);
	if (!hw->io_base) {
		dev_err(&pdev->dev, "Can't map configuration register space\n");
		ret = -ENOMEM;
		goto err_clr_master;
	}

	hdev->num_req_vfs = pci_sriov_get_totalvfs(pdev);

	return 0;
err_clr_master:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);

	return ret;
}

static void hclge_pci_uninit(struct hclge_dev *hdev)
{
	struct pci_dev *pdev = hdev->pdev;

	pcim_iounmap(pdev, hdev->hw.io_base);
	pci_free_irq_vectors(pdev);
	pci_clear_master(pdev);
	pci_release_mem_regions(pdev);
	pci_disable_device(pdev);
}

static void hclge_state_init(struct hclge_dev *hdev)
{
	set_bit(HCLGE_STATE_SERVICE_INITED, &hdev->state);
	set_bit(HCLGE_STATE_DOWN, &hdev->state);
	clear_bit(HCLGE_STATE_RST_SERVICE_SCHED, &hdev->state);
	clear_bit(HCLGE_STATE_RST_HANDLING, &hdev->state);
	clear_bit(HCLGE_STATE_MBX_SERVICE_SCHED, &hdev->state);
	clear_bit(HCLGE_STATE_MBX_HANDLING, &hdev->state);
}

static void hclge_state_uninit(struct hclge_dev *hdev)
{
	set_bit(HCLGE_STATE_DOWN, &hdev->state);

	if (hdev->service_timer.function)
		del_timer_sync(&hdev->service_timer);
	if (hdev->reset_timer.function)
		del_timer_sync(&hdev->reset_timer);
	if (hdev->service_task.func)
		cancel_work_sync(&hdev->service_task);
	if (hdev->rst_service_task.func)
		cancel_work_sync(&hdev->rst_service_task);
	if (hdev->mbx_service_task.func)
		cancel_work_sync(&hdev->mbx_service_task);
}

static void hclge_flr_prepare(struct hnae3_ae_dev *ae_dev)
{
#define HCLGE_FLR_WAIT_MS	100
#define HCLGE_FLR_WAIT_CNT	50
	struct hclge_dev *hdev = ae_dev->priv;
	int cnt = 0;

	clear_bit(HNAE3_FLR_DOWN, &hdev->flr_state);
	clear_bit(HNAE3_FLR_DONE, &hdev->flr_state);
	set_bit(HNAE3_FLR_RESET, &hdev->default_reset_request);
	hclge_reset_event(hdev->pdev, NULL);

	while (!test_bit(HNAE3_FLR_DOWN, &hdev->flr_state) &&
	       cnt++ < HCLGE_FLR_WAIT_CNT)
		msleep(HCLGE_FLR_WAIT_MS);

	if (!test_bit(HNAE3_FLR_DOWN, &hdev->flr_state))
		dev_err(&hdev->pdev->dev,
			"flr wait down timeout: %d\n", cnt);
}

static void hclge_flr_done(struct hnae3_ae_dev *ae_dev)
{
	struct hclge_dev *hdev = ae_dev->priv;

	set_bit(HNAE3_FLR_DONE, &hdev->flr_state);
}

static int hclge_init_ae_dev(struct hnae3_ae_dev *ae_dev)
{
	struct pci_dev *pdev = ae_dev->pdev;
	struct hclge_dev *hdev;
	int ret;

	hdev = devm_kzalloc(&pdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev) {
		ret = -ENOMEM;
		goto out;
	}

	hdev->pdev = pdev;
	hdev->ae_dev = ae_dev;
	hdev->reset_type = HNAE3_NONE_RESET;
	hdev->reset_level = HNAE3_FUNC_RESET;
	ae_dev->priv = hdev;
	hdev->mps = ETH_FRAME_LEN + ETH_FCS_LEN + 2 * VLAN_HLEN;

	mutex_init(&hdev->vport_lock);
	mutex_init(&hdev->vport_cfg_mutex);

	ret = hclge_pci_init(hdev);
	if (ret) {
		dev_err(&pdev->dev, "PCI init failed\n");
		goto out;
	}

	/* Firmware command queue initialize */
	ret = hclge_cmd_queue_init(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Cmd queue init failed, ret = %d.\n", ret);
		goto err_pci_uninit;
	}

	/* Firmware command initialize */
	ret = hclge_cmd_init(hdev);
	if (ret)
		goto err_cmd_uninit;

	ret = hclge_get_cap(hdev);
	if (ret) {
		dev_err(&pdev->dev, "get hw capability error, ret = %d.\n",
			ret);
		goto err_cmd_uninit;
	}

	ret = hclge_configure(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Configure dev error, ret = %d.\n", ret);
		goto err_cmd_uninit;
	}

	ret = hclge_init_msi(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Init MSI/MSI-X error, ret = %d.\n", ret);
		goto err_cmd_uninit;
	}

	ret = hclge_misc_irq_init(hdev);
	if (ret) {
		dev_err(&pdev->dev,
			"Misc IRQ(vector0) init error, ret = %d.\n",
			ret);
		goto err_msi_uninit;
	}

	ret = hclge_alloc_tqps(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Allocate TQPs error, ret = %d.\n", ret);
		goto err_msi_irq_uninit;
	}

	ret = hclge_alloc_vport(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Allocate vport error, ret = %d.\n", ret);
		goto err_msi_irq_uninit;
	}

	ret = hclge_map_tqp(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Map tqp error, ret = %d.\n", ret);
		goto err_msi_irq_uninit;
	}

	if (hdev->hw.mac.media_type == HNAE3_MEDIA_TYPE_COPPER) {
		ret = hclge_mac_mdio_config(hdev);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"mdio config fail ret=%d\n", ret);
			goto err_msi_irq_uninit;
		}
	}

	ret = hclge_init_umv_space(hdev);
	if (ret) {
		dev_err(&pdev->dev, "umv space init error, ret=%d.\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = hclge_mac_init(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Mac init error, ret = %d\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = hclge_config_tso(hdev, HCLGE_TSO_MSS_MIN, HCLGE_TSO_MSS_MAX);
	if (ret) {
		dev_err(&pdev->dev, "Enable tso fail, ret =%d\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = hclge_config_gro(hdev, true);
	if (ret)
		goto err_mdiobus_unreg;

	ret = hclge_init_vlan_config(hdev);
	if (ret) {
		dev_err(&pdev->dev, "VLAN init fail, ret =%d\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = hclge_tm_schd_init(hdev);
	if (ret) {
		dev_err(&pdev->dev, "tm schd init fail, ret =%d\n", ret);
		goto err_mdiobus_unreg;
	}

	hclge_rss_init_cfg(hdev);
	ret = hclge_rss_init_hw(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Rss init fail, ret =%d\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = init_mgr_tbl(hdev);
	if (ret) {
		dev_err(&pdev->dev, "manager table init fail, ret =%d\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = hclge_init_fd_config(hdev);
	if (ret) {
		dev_err(&pdev->dev,
			"fd table init fail, ret=%d\n", ret);
		goto err_mdiobus_unreg;
	}

	ret = hclge_hw_error_set_state(hdev, true);
	if (ret) {
		dev_err(&pdev->dev,
			"fail(%d) to enable hw error interrupts\n", ret);
		goto err_mdiobus_unreg;
	}

	hclge_dcb_ops_set(hdev);

	timer_setup(&hdev->service_timer, hclge_service_timer, 0);
	timer_setup(&hdev->reset_timer, hclge_reset_timer, 0);
	INIT_WORK(&hdev->service_task, hclge_service_task);
	INIT_WORK(&hdev->rst_service_task, hclge_reset_service_task);
	INIT_WORK(&hdev->mbx_service_task, hclge_mailbox_service_task);

	hclge_clear_all_event_cause(hdev);

	/* Enable MISC vector(vector0) */
	hclge_enable_vector(&hdev->misc_vector, true);

	hclge_state_init(hdev);
	hdev->last_reset_time = jiffies;

	pr_info("%s driver initialization finished.\n", HCLGE_DRIVER_NAME);
	return 0;

err_mdiobus_unreg:
	if (hdev->hw.mac.phydev)
		mdiobus_unregister(hdev->hw.mac.mdio_bus);
err_msi_irq_uninit:
	hclge_misc_irq_uninit(hdev);
err_msi_uninit:
	pci_free_irq_vectors(pdev);
err_cmd_uninit:
	hclge_cmd_uninit(hdev);
err_pci_uninit:
	pcim_iounmap(pdev, hdev->hw.io_base);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
out:
	return ret;
}

static void hclge_stats_clear(struct hclge_dev *hdev)
{
	memset(&hdev->hw_stats, 0, sizeof(hdev->hw_stats));
}

static void hclge_reset_vport_state(struct hclge_dev *hdev)
{
	struct hclge_vport *vport = hdev->vport;
	int i;

	for (i = 0; i < hdev->num_alloc_vport; i++) {
		hclge_vport_start(vport);
		vport++;
	}
}

static int hclge_reset_ae_dev(struct hnae3_ae_dev *ae_dev)
{
	struct hclge_dev *hdev = ae_dev->priv;
	struct pci_dev *pdev = ae_dev->pdev;
	int ret;

	set_bit(HCLGE_STATE_DOWN, &hdev->state);

	hclge_stats_clear(hdev);
	memset(hdev->vlan_table, 0, sizeof(hdev->vlan_table));

	ret = hclge_cmd_init(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Cmd queue init failed\n");
		return ret;
	}

	ret = hclge_map_tqp(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Map tqp error, ret = %d.\n", ret);
		return ret;
	}

	hclge_reset_umv_space(hdev);

	ret = hclge_mac_init(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Mac init error, ret = %d\n", ret);
		return ret;
	}

	ret = hclge_config_tso(hdev, HCLGE_TSO_MSS_MIN, HCLGE_TSO_MSS_MAX);
	if (ret) {
		dev_err(&pdev->dev, "Enable tso fail, ret =%d\n", ret);
		return ret;
	}

	ret = hclge_config_gro(hdev, true);
	if (ret)
		return ret;

	ret = hclge_init_vlan_config(hdev);
	if (ret) {
		dev_err(&pdev->dev, "VLAN init fail, ret =%d\n", ret);
		return ret;
	}

	ret = hclge_tm_init_hw(hdev, true);
	if (ret) {
		dev_err(&pdev->dev, "tm init hw fail, ret =%d\n", ret);
		return ret;
	}

	ret = hclge_rss_init_hw(hdev);
	if (ret) {
		dev_err(&pdev->dev, "Rss init fail, ret =%d\n", ret);
		return ret;
	}

	ret = hclge_init_fd_config(hdev);
	if (ret) {
		dev_err(&pdev->dev,
			"fd table init fail, ret=%d\n", ret);
		return ret;
	}

	/* Re-enable the hw error interrupts because
	 * the interrupts get disabled on core/global reset.
	 */
	ret = hclge_hw_error_set_state(hdev, true);
	if (ret) {
		dev_err(&pdev->dev,
			"fail(%d) to re-enable HNS hw error interrupts\n", ret);
		return ret;
	}

	hclge_reset_vport_state(hdev);

	dev_info(&pdev->dev, "Reset done, %s driver initialization finished.\n",
		 HCLGE_DRIVER_NAME);

	return 0;
}

static void hclge_uninit_ae_dev(struct hnae3_ae_dev *ae_dev)
{
	struct hclge_dev *hdev = ae_dev->priv;
	struct hclge_mac *mac = &hdev->hw.mac;

	hclge_state_uninit(hdev);

	if (mac->phydev)
		mdiobus_unregister(mac->mdio_bus);

	hclge_uninit_umv_space(hdev);

	/* Disable MISC vector(vector0) */
	hclge_enable_vector(&hdev->misc_vector, false);
	synchronize_irq(hdev->misc_vector.vector_irq);

	hclge_hw_error_set_state(hdev, false);
	hclge_cmd_uninit(hdev);
	hclge_misc_irq_uninit(hdev);
	hclge_pci_uninit(hdev);
	mutex_destroy(&hdev->vport_lock);
	hclge_uninit_vport_mac_table(hdev);
	hclge_uninit_vport_vlan_table(hdev);
	mutex_destroy(&hdev->vport_cfg_mutex);
	ae_dev->priv = NULL;
}

static u32 hclge_get_max_channels(struct hnae3_handle *handle)
{
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return min_t(u32, hdev->rss_size_max,
		     vport->alloc_tqps / kinfo->num_tc);
}

static void hclge_get_channels(struct hnae3_handle *handle,
			       struct ethtool_channels *ch)
{
	ch->max_combined = hclge_get_max_channels(handle);
	ch->other_count = 1;
	ch->max_other = 1;
	ch->combined_count = handle->kinfo.rss_size;
}

static void hclge_get_tqps_and_rss_info(struct hnae3_handle *handle,
					u16 *alloc_tqps, u16 *max_rss_size)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	*alloc_tqps = vport->alloc_tqps;
	*max_rss_size = hdev->rss_size_max;
}

static int hclge_set_channels(struct hnae3_handle *handle, u32 new_tqps_num,
			      bool rxfh_configured)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hnae3_knic_private_info *kinfo = &vport->nic.kinfo;
	struct hclge_dev *hdev = vport->back;
	int cur_rss_size = kinfo->rss_size;
	int cur_tqps = kinfo->num_tqps;
	u16 tc_offset[HCLGE_MAX_TC_NUM];
	u16 tc_valid[HCLGE_MAX_TC_NUM];
	u16 tc_size[HCLGE_MAX_TC_NUM];
	u16 roundup_size;
	u32 *rss_indir;
	int ret, i;

	kinfo->req_rss_size = new_tqps_num;

	ret = hclge_tm_vport_map_update(hdev);
	if (ret) {
		dev_err(&hdev->pdev->dev, "tm vport map fail, ret =%d\n", ret);
		return ret;
	}

	roundup_size = roundup_pow_of_two(kinfo->rss_size);
	roundup_size = ilog2(roundup_size);
	/* Set the RSS TC mode according to the new RSS size */
	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		tc_valid[i] = 0;

		if (!(hdev->hw_tc_map & BIT(i)))
			continue;

		tc_valid[i] = 1;
		tc_size[i] = roundup_size;
		tc_offset[i] = kinfo->rss_size * i;
	}
	ret = hclge_set_rss_tc_mode(hdev, tc_valid, tc_size, tc_offset);
	if (ret)
		return ret;

	/* RSS indirection table has been configuared by user */
	if (rxfh_configured)
		goto out;

	/* Reinitializes the rss indirect table according to the new RSS size */
	rss_indir = kcalloc(HCLGE_RSS_IND_TBL_SIZE, sizeof(u32), GFP_KERNEL);
	if (!rss_indir)
		return -ENOMEM;

	for (i = 0; i < HCLGE_RSS_IND_TBL_SIZE; i++)
		rss_indir[i] = i % kinfo->rss_size;

	ret = hclge_set_rss(handle, rss_indir, NULL, 0);
	if (ret)
		dev_err(&hdev->pdev->dev, "set rss indir table fail, ret=%d\n",
			ret);

	kfree(rss_indir);

out:
	if (!ret)
		dev_info(&hdev->pdev->dev,
			 "Channels changed, rss_size from %d to %d, tqps from %d to %d",
			 cur_rss_size, kinfo->rss_size,
			 cur_tqps, kinfo->rss_size * kinfo->num_tc);

	return ret;
}

static int hclge_get_regs_num(struct hclge_dev *hdev, u32 *regs_num_32_bit,
			      u32 *regs_num_64_bit)
{
	struct hclge_desc desc;
	u32 total_num;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_QUERY_REG_NUM, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Query register number cmd failed, ret = %d.\n", ret);
		return ret;
	}

	*regs_num_32_bit = le32_to_cpu(desc.data[0]);
	*regs_num_64_bit = le32_to_cpu(desc.data[1]);

	total_num = *regs_num_32_bit + *regs_num_64_bit;
	if (!total_num)
		return -EINVAL;

	return 0;
}

static int hclge_get_32_bit_regs(struct hclge_dev *hdev, u32 regs_num,
				 void *data)
{
#define HCLGE_32_BIT_REG_RTN_DATANUM 8

	struct hclge_desc *desc;
	u32 *reg_val = data;
	__le32 *desc_data;
	int cmd_num;
	int i, k, n;
	int ret;

	if (regs_num == 0)
		return 0;

	cmd_num = DIV_ROUND_UP(regs_num + 2, HCLGE_32_BIT_REG_RTN_DATANUM);
	desc = kcalloc(cmd_num, sizeof(struct hclge_desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_QUERY_32_BIT_REG, true);
	ret = hclge_cmd_send(&hdev->hw, desc, cmd_num);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Query 32 bit register cmd failed, ret = %d.\n", ret);
		kfree(desc);
		return ret;
	}

	for (i = 0; i < cmd_num; i++) {
		if (i == 0) {
			desc_data = (__le32 *)(&desc[i].data[0]);
			n = HCLGE_32_BIT_REG_RTN_DATANUM - 2;
		} else {
			desc_data = (__le32 *)(&desc[i]);
			n = HCLGE_32_BIT_REG_RTN_DATANUM;
		}
		for (k = 0; k < n; k++) {
			*reg_val++ = le32_to_cpu(*desc_data++);

			regs_num--;
			if (!regs_num)
				break;
		}
	}

	kfree(desc);
	return 0;
}

static int hclge_get_64_bit_regs(struct hclge_dev *hdev, u32 regs_num,
				 void *data)
{
#define HCLGE_64_BIT_REG_RTN_DATANUM 4

	struct hclge_desc *desc;
	u64 *reg_val = data;
	__le64 *desc_data;
	int cmd_num;
	int i, k, n;
	int ret;

	if (regs_num == 0)
		return 0;

	cmd_num = DIV_ROUND_UP(regs_num + 1, HCLGE_64_BIT_REG_RTN_DATANUM);
	desc = kcalloc(cmd_num, sizeof(struct hclge_desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_QUERY_64_BIT_REG, true);
	ret = hclge_cmd_send(&hdev->hw, desc, cmd_num);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Query 64 bit register cmd failed, ret = %d.\n", ret);
		kfree(desc);
		return ret;
	}

	for (i = 0; i < cmd_num; i++) {
		if (i == 0) {
			desc_data = (__le64 *)(&desc[i].data[0]);
			n = HCLGE_64_BIT_REG_RTN_DATANUM - 1;
		} else {
			desc_data = (__le64 *)(&desc[i]);
			n = HCLGE_64_BIT_REG_RTN_DATANUM;
		}
		for (k = 0; k < n; k++) {
			*reg_val++ = le64_to_cpu(*desc_data++);

			regs_num--;
			if (!regs_num)
				break;
		}
	}

	kfree(desc);
	return 0;
}

#define MAX_SEPARATE_NUM	4
#define SEPARATOR_VALUE		0xFFFFFFFF
#define REG_NUM_PER_LINE	4
#define REG_LEN_PER_LINE	(REG_NUM_PER_LINE * sizeof(u32))

static int hclge_get_regs_len(struct hnae3_handle *handle)
{
	int cmdq_lines, common_lines, ring_lines, tqp_intr_lines;
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	u32 regs_num_32_bit, regs_num_64_bit;
	int ret;

	ret = hclge_get_regs_num(hdev, &regs_num_32_bit, &regs_num_64_bit);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Get register number failed, ret = %d.\n", ret);
		return -EOPNOTSUPP;
	}

	cmdq_lines = sizeof(cmdq_reg_addr_list) / REG_LEN_PER_LINE + 1;
	common_lines = sizeof(common_reg_addr_list) / REG_LEN_PER_LINE + 1;
	ring_lines = sizeof(ring_reg_addr_list) / REG_LEN_PER_LINE + 1;
	tqp_intr_lines = sizeof(tqp_intr_reg_addr_list) / REG_LEN_PER_LINE + 1;

	return (cmdq_lines + common_lines + ring_lines * kinfo->num_tqps +
		tqp_intr_lines * (hdev->num_msi_used - 1)) * REG_LEN_PER_LINE +
		regs_num_32_bit * sizeof(u32) + regs_num_64_bit * sizeof(u64);
}

static void hclge_get_regs(struct hnae3_handle *handle, u32 *version,
			   void *data)
{
	struct hnae3_knic_private_info *kinfo = &handle->kinfo;
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	u32 regs_num_32_bit, regs_num_64_bit;
	int i, j, reg_um, separator_num;
	u32 *reg = data;
	int ret;

	*version = hdev->fw_version;

	ret = hclge_get_regs_num(hdev, &regs_num_32_bit, &regs_num_64_bit);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Get register number failed, ret = %d.\n", ret);
		return;
	}

	/* fetching per-PF registers valus from PF PCIe register space */
	reg_um = sizeof(cmdq_reg_addr_list) / sizeof(u32);
	separator_num = MAX_SEPARATE_NUM - reg_um % REG_NUM_PER_LINE;
	for (i = 0; i < reg_um; i++)
		*reg++ = hclge_read_dev(&hdev->hw, cmdq_reg_addr_list[i]);
	for (i = 0; i < separator_num; i++)
		*reg++ = SEPARATOR_VALUE;

	reg_um = sizeof(common_reg_addr_list) / sizeof(u32);
	separator_num = MAX_SEPARATE_NUM - reg_um % REG_NUM_PER_LINE;
	for (i = 0; i < reg_um; i++)
		*reg++ = hclge_read_dev(&hdev->hw, common_reg_addr_list[i]);
	for (i = 0; i < separator_num; i++)
		*reg++ = SEPARATOR_VALUE;

	reg_um = sizeof(ring_reg_addr_list) / sizeof(u32);
	separator_num = MAX_SEPARATE_NUM - reg_um % REG_NUM_PER_LINE;
	for (j = 0; j < kinfo->num_tqps; j++) {
		for (i = 0; i < reg_um; i++)
			*reg++ = hclge_read_dev(&hdev->hw,
						ring_reg_addr_list[i] +
						0x200 * j);
		for (i = 0; i < separator_num; i++)
			*reg++ = SEPARATOR_VALUE;
	}

	reg_um = sizeof(tqp_intr_reg_addr_list) / sizeof(u32);
	separator_num = MAX_SEPARATE_NUM - reg_um % REG_NUM_PER_LINE;
	for (j = 0; j < hdev->num_msi_used - 1; j++) {
		for (i = 0; i < reg_um; i++)
			*reg++ = hclge_read_dev(&hdev->hw,
						tqp_intr_reg_addr_list[i] +
						4 * j);
		for (i = 0; i < separator_num; i++)
			*reg++ = SEPARATOR_VALUE;
	}

	/* fetching PF common registers values from firmware */
	ret = hclge_get_32_bit_regs(hdev, regs_num_32_bit, reg);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"Get 32 bit register failed, ret = %d.\n", ret);
		return;
	}

	reg += regs_num_32_bit;
	ret = hclge_get_64_bit_regs(hdev, regs_num_64_bit, reg);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Get 64 bit register failed, ret = %d.\n", ret);
}

static int hclge_set_led_status(struct hclge_dev *hdev, u8 locate_led_status)
{
	struct hclge_set_led_state_cmd *req;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_LED_STATUS_CFG, false);

	req = (struct hclge_set_led_state_cmd *)desc.data;
	hnae3_set_field(req->locate_led_config, HCLGE_LED_LOCATE_STATE_M,
			HCLGE_LED_LOCATE_STATE_S, locate_led_status);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"Send set led state cmd error, ret =%d\n", ret);

	return ret;
}

enum hclge_led_status {
	HCLGE_LED_OFF,
	HCLGE_LED_ON,
	HCLGE_LED_NO_CHANGE = 0xFF,
};

static int hclge_set_led_id(struct hnae3_handle *handle,
			    enum ethtool_phys_id_state status)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	switch (status) {
	case ETHTOOL_ID_ACTIVE:
		return hclge_set_led_status(hdev, HCLGE_LED_ON);
	case ETHTOOL_ID_INACTIVE:
		return hclge_set_led_status(hdev, HCLGE_LED_OFF);
	default:
		return -EINVAL;
	}
}

static void hclge_get_link_mode(struct hnae3_handle *handle,
				unsigned long *supported,
				unsigned long *advertising)
{
	unsigned int size = BITS_TO_LONGS(__ETHTOOL_LINK_MODE_MASK_NBITS);
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	unsigned int idx = 0;

	for (; idx < size; idx++) {
		supported[idx] = hdev->hw.mac.supported[idx];
		advertising[idx] = hdev->hw.mac.advertising[idx];
	}
}

static int hclge_gro_en(struct hnae3_handle *handle, bool enable)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	return hclge_config_gro(hdev, enable);
}

static const struct hnae3_ae_ops hclge_ops = {
	.init_ae_dev = hclge_init_ae_dev,
	.uninit_ae_dev = hclge_uninit_ae_dev,
	.flr_prepare = hclge_flr_prepare,
	.flr_done = hclge_flr_done,
	.init_client_instance = hclge_init_client_instance,
	.uninit_client_instance = hclge_uninit_client_instance,
	.map_ring_to_vector = hclge_map_ring_to_vector,
	.unmap_ring_from_vector = hclge_unmap_ring_frm_vector,
	.get_vector = hclge_get_vector,
	.put_vector = hclge_put_vector,
	.set_promisc_mode = hclge_set_promisc_mode,
	.set_loopback = hclge_set_loopback,
	.start = hclge_ae_start,
	.stop = hclge_ae_stop,
	.client_start = hclge_client_start,
	.client_stop = hclge_client_stop,
	.get_status = hclge_get_status,
	.get_ksettings_an_result = hclge_get_ksettings_an_result,
	.update_speed_duplex_h = hclge_update_speed_duplex_h,
	.cfg_mac_speed_dup_h = hclge_cfg_mac_speed_dup_h,
	.get_media_type = hclge_get_media_type,
	.get_rss_key_size = hclge_get_rss_key_size,
	.get_rss_indir_size = hclge_get_rss_indir_size,
	.get_rss = hclge_get_rss,
	.set_rss = hclge_set_rss,
	.set_rss_tuple = hclge_set_rss_tuple,
	.get_rss_tuple = hclge_get_rss_tuple,
	.get_tc_size = hclge_get_tc_size,
	.get_mac_addr = hclge_get_mac_addr,
	.set_mac_addr = hclge_set_mac_addr,
	.do_ioctl = hclge_do_ioctl,
	.add_uc_addr = hclge_add_uc_addr,
	.rm_uc_addr = hclge_rm_uc_addr,
	.add_mc_addr = hclge_add_mc_addr,
	.rm_mc_addr = hclge_rm_mc_addr,
	.set_autoneg = hclge_set_autoneg,
	.get_autoneg = hclge_get_autoneg,
	.get_pauseparam = hclge_get_pauseparam,
	.set_pauseparam = hclge_set_pauseparam,
	.set_mtu = hclge_set_mtu,
	.reset_queue = hclge_reset_tqp,
	.get_stats = hclge_get_stats,
	.update_stats = hclge_update_stats,
	.get_strings = hclge_get_strings,
	.get_sset_count = hclge_get_sset_count,
	.get_fw_version = hclge_get_fw_version,
	.get_mdix_mode = hclge_get_mdix_mode,
	.enable_vlan_filter = hclge_enable_vlan_filter,
	.set_vlan_filter = hclge_set_vlan_filter,
	.set_vf_vlan_filter = hclge_set_vf_vlan_filter,
	.enable_hw_strip_rxvtag = hclge_en_hw_strip_rxvtag,
	.reset_event = hclge_reset_event,
	.set_default_reset_request = hclge_set_def_reset_request,
	.get_tqps_and_rss_info = hclge_get_tqps_and_rss_info,
	.set_channels = hclge_set_channels,
	.get_channels = hclge_get_channels,
	.get_regs_len = hclge_get_regs_len,
	.get_regs = hclge_get_regs,
	.set_led_id = hclge_set_led_id,
	.get_link_mode = hclge_get_link_mode,
	.add_fd_entry = hclge_add_fd_entry,
	.del_fd_entry = hclge_del_fd_entry,
	.del_all_fd_entries = hclge_del_all_fd_entries,
	.get_fd_rule_cnt = hclge_get_fd_rule_cnt,
	.get_fd_rule_info = hclge_get_fd_rule_info,
	.get_fd_all_rules = hclge_get_all_rules,
	.restore_fd_rules = hclge_restore_fd_entries,
	.enable_fd = hclge_enable_fd,
	.dbg_run_cmd = hclge_dbg_run_cmd,
	.handle_hw_ras_error = hclge_handle_hw_ras_error,
	.get_hw_reset_stat = hclge_get_hw_reset_stat,
	.ae_dev_resetting = hclge_ae_dev_resetting,
	.ae_dev_reset_cnt = hclge_ae_dev_reset_cnt,
	.set_gro_en = hclge_gro_en,
	.get_global_queue_id = hclge_covert_handle_qid_global,
	.set_timer_task = hclge_set_timer_task,
	.mac_connect_phy = hclge_mac_connect_phy,
	.mac_disconnect_phy = hclge_mac_disconnect_phy,
};

static struct hnae3_ae_algo ae_algo = {
	.ops = &hclge_ops,
	.pdev_id_table = ae_algo_pci_tbl,
};

static int hclge_init(void)
{
	pr_info("%s is initializing\n", HCLGE_NAME);

	hnae3_register_ae_algo(&ae_algo);

	return 0;
}

static void hclge_exit(void)
{
	hnae3_unregister_ae_algo(&ae_algo);
}
module_init(hclge_init);
module_exit(hclge_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_DESCRIPTION("HCLGE Driver");
MODULE_VERSION(HCLGE_MOD_VERSION);
