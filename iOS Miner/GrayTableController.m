//
//  GrayTableController.m
//  iOS Miner
//
//  Created by Ethan Dye on 1/16/18.
//  Copyright © 2018 Ethan Dye. All rights reserved.
//
//
//  Credits: Orignal Source:https://github.com/limneos/MobileMiner/
//

#include "GrayTableController.h"
#include "CustomCell.h"

@implementation GrayTableController
-(void)loadView{
    
    self.tableView=[[UITableView alloc] initWithFrame:[[UIScreen mainScreen] bounds] style:UITableViewStylePlain];
    self.tableView.tableFooterView = [[UIView alloc] init];
    self.tableView.backgroundColor=[UIColor colorWithRed:0.50 green:0.55 blue:0.55 alpha:1.0];
    self.tableView.delegate=self;
    self.tableView.dataSource=self;
    self.tableView.separatorStyle=UITableViewCellSeparatorStyleNone;
    
}
-(NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section{
    return 0;
}
-(NSInteger)numberOfSectionsInTableView:(UITableView *)tableView{
    return 1;
}
-(CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath{
    return 70;
}
-(void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath{
    [tableView deselectRowAtIndexPath:indexPath animated:NO];
}
-(id)tableView:(UITableView *)tableView cellForRowAtIndexPath:(nonnull NSIndexPath *)indexPath  {
    
    NSString *REUSEID = [NSString stringWithFormat:@"ACELL-%d-%d",(int)indexPath.section,(int)indexPath.row];
    CustomCell *cell=[tableView dequeueReusableCellWithIdentifier:REUSEID];
    
    if (!cell){
        cell=[[CustomCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:REUSEID];
    }
    return (UITableViewCell *)cell;
}
-(void)viewDidAppear:(BOOL)animated{
    [super viewDidAppear:animated];
    [[self tableView] reloadData];
}
@end
